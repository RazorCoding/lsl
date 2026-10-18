/*
 * lsl_upscale.c -- the layer's present-time upscaling engine.
 *
 * What this does, at present time, on the application's own queue:
 *
 *   swapchain image (N x M, but only the top-left W x H holds the app's frame)
 *     -> copy to buf_in
 *     -> scale   (shaders/scale.comp)                W x H -> N x M
 *     -> [refiner]                                    N x M -> N x M
 *     -> copy back over the whole swapchain image
 *
 * `scale` is where the method lives.  A spatial method (nearest, bilinear,
 * cubic, lanczos2, lanczos3) is the whole upscale, so it runs `scale` with that
 * method's id and nothing else.  FSR1 is EASU, so `fsr1` is `scale` with mode 4;
 * the methods that refine the EASU result pick it with mode 4 and then run a
 * second pass at destination resolution:
 *
 *   fsr1rcas   scale(EASU) -> rcas            (shaders/rcas.comp)
 *   nis        scale(EASU) -> nis             (shaders/nis.comp)
 *   cnn        scale(EASU) -> 4 conv layers   (shaders/cnn.comp)
 *
 * The refiner is what makes the pass count per method visible: the barrier plan
 * below is written once for each of the three.
 *
 * Buffers rather than sampled images, because the Phase 1 shaders -- and the CPU
 * reference they are checked against -- work on tightly packed storage buffers.
 * Reusing the exact same shaders means the layer's output can be compared pixel
 * for pixel against `lsl-upscale` fed the same input, which is how this gets
 * verified without looking at the screen (see tools/upscale-check.sh).  That
 * comparison is run for every method, not just the default.
 *
 * Two pixel layouts, chosen from the swapchain format:
 *
 *   RGBA8   (R8G8B8A8_*, B8G8R8A8_*)   one uint per pixel -- the Phase 1 layout
 *   16F     (R16G16B16A16_SFLOAT)      four half floats, two uints per pixel
 *
 * The 16F path exists because a Wayland session may hand that format out --
 * vkcube gets it on this host -- and because HDR swapchains use it.  Both are
 * the *same* shader source built twice, with -DLSL_IO_16F selecting the load
 * and store.
 *
 * The GPU buffers are double buffered.  Reusing a slot before its previous
 * submission finished would be a write-after-read race; the fence that guards
 * reuse is normally already signalled, so the wait does not cost anything.
 */

#include "lsl_layer.h"
#include "lsl_method.h"
#include "anime4k_restore_s.h"
#include "spv_blob_c.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- pixel formats */

int lsl_format_is_16f(VkFormat f) { return f == VK_FORMAT_R16G16B16A16_SFLOAT; }

int lsl_format_is_bgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

int lsl_format_supported(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 1;
        default:
            return 0;
    }
}

uint32_t lsl_format_texel_bytes(VkFormat f) {
    return lsl_format_is_16f(f) ? 8u : 4u;
}

static const char *format_name(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        default: return "unsupported";
    }
}

/* --------------------------------------------------------------- engine state */

/* Must match the push_constant block in shaders/scale.comp, shaders/rcas.comp and
 * shaders/nis.comp. */
typedef struct {
    uint32_t srcW, srcH, dstW, dstH;
    uint32_t mode;      /* an LSL_MODE_* value for scale.comp; ignored by the refiners */
    float sharpness;
    uint32_t pad0, pad1;
} PushConst;

/* shaders/cnn.comp has its own five-word push block, because it is dispatched
 * once per layer and each layer is described by its weight offsets and flags. */
typedef struct {
    uint32_t W, H;
    uint32_t woff, boff;   /* this layer's first matrix and bias, as float indices */
    uint32_t flags;        /* LSL_CNN_* */
} CnnPushConst;

/* Two in flight.  A slot's buffers and command buffer may not be touched again
 * until its fence signals, and the fence for an older slot is normally already
 * signalled, so reuse does not cost a stall. */
#define UP_SLOTS 2

/* shaders/scale.comp, shaders/rcas.comp and shaders/nis.comp all use 16x16
 * workgroups; shaders/cnn.comp uses 8x8. */
#define UP_GROUPS(n) (((n) + 15u) / 16u)
#define UP_GROUPS_CNN(n) (((n) + 7u) / 8u)

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    int submitted;
    VkBuffer in, mid;          /* plane_bytes each */
    VkDeviceMemory in_mem, mid_mem;
    VkDescriptorSet set_scale;   /* in  -> mid : every method's first pass */
    VkDescriptorSet set_refine;  /* mid -> in  : rcas / nis */
    /* The CNN ping-pongs two float activations.  Only allocated -- like
     * set_cnn -- when the method needs them, so the default costs no more
     * memory than it did before the methods existed. */
    VkBuffer cnn_a, cnn_b;
    VkDeviceMemory cnn_a_mem, cnn_b_mem;
    VkDescriptorSet set_cnn;   /* mid, cnn_a, cnn_b, weights, in */
} UpSlot;

typedef struct {
    int valid;
    /* The device the engine's objects belong to.  Applications normally have
     * one, but a process may hold two alive at once (vkcube does), and the
     * engine has to be rebuilt rather than reused when the device changes. */
    VkDevice dev;
    /* The next-chain entry points, resolved for `dev`.  Resolving them for
     * whichever device was created last and then calling a command buffer that
     * lives on `dev` through that other device's chain is a dispatch mismatch:
     * the driver tolerates the loader's device-agnostic trampolines, but the
     * validation layer aborts on it. */
    LslDevFns fn;
    LslUpscaleShape shape;
    uint32_t mode;        /* shape.mode, resolved: never LSL_MODE_AUTO here */
    int declared_auto;    /* shape.mode was LSL_MODE_AUTO, for the log line */
    uint32_t family;
    uint32_t texel;
    int is16;
    VkDeviceSize plane_bytes;

    VkDescriptorSetLayout dsl;      /* two storage buffers */
    VkPipelineLayout playout;
    VkPipeline pipe_scale;
    VkPipeline pipe_rcas;
    VkPipeline pipe_nis;
    /* shaders/cnn.comp samples five buffers (base, A, B, weights, out), so it
     * needs its own set layout and pipeline layout. */
    VkDescriptorSetLayout dsl_cnn;
    VkPipelineLayout playout_cnn;
    VkPipeline pipe_cnn;
    /* The 4 KB weight table, host visible: it is read-only and tiny enough to
     * stay in the GPU's cache, and keeping it in host memory avoids needing a
     * queue to stage it at build time. */
    VkBuffer wts;
    VkDeviceMemory wts_mem;
    VkDescriptorPool dpool;

    UpSlot slot[UP_SLOTS];
    int next_slot;

    VkCommandPool pool;
    VkBuffer stage;            /* host visible, for the verification dump */
    VkDeviceMemory stage_mem;
    VkDeviceSize stage_out_off;
    VkDeviceSize stage_easu_off;
} Upscale;

static Upscale g_up;

/* The device the engine's objects live on.  The layer's entry points are
 * global, so a process with more than one live device (vkcube creates two on
 * this host) makes swapchains on both; the engine follows the swapchain's
 * device and the present path checks it before running. */
static VkDevice up_dev(void) { return g_up.dev ? g_up.dev : g_device; }

/* Set once, never cleared: a shape change rebuilds the engine but must not cause
 * a second dump. */
static int g_dumped;

static int up_have_fns(void) {
    return up_dev() != VK_NULL_HANDLE && g_have_memprops &&
           g_up.fn.create_command_pool && g_up.fn.destroy_command_pool &&
           g_up.fn.allocate_command_buffers && g_up.fn.reset_command_buffer &&
           g_up.fn.begin_command_buffer && g_up.fn.end_command_buffer &&
           g_up.fn.cmd_pipeline_barrier && g_up.fn.cmd_copy_image_to_buffer &&
           g_up.fn.cmd_copy_buffer_to_image && g_up.fn.cmd_copy_buffer &&
           g_up.fn.cmd_bind_pipeline && g_up.fn.cmd_bind_descriptor_sets &&
           g_up.fn.cmd_push_constants && g_up.fn.cmd_dispatch &&
           g_up.fn.create_fence && g_up.fn.destroy_fence && g_up.fn.reset_fences &&
           g_up.fn.wait_for_fences && g_up.fn.queue_submit &&
           g_up.fn.map_memory && g_up.fn.unmap_memory &&
           g_up.fn.create_buffer && g_up.fn.destroy_buffer &&
           g_up.fn.get_buffer_memory_requirements && g_up.fn.allocate_memory &&
           g_up.fn.free_memory && g_up.fn.bind_buffer_memory &&
           g_up.fn.create_shader_module && g_up.fn.destroy_shader_module &&
           g_up.fn.create_descriptor_set_layout && g_up.fn.destroy_descriptor_set_layout &&
           g_up.fn.create_pipeline_layout && g_up.fn.destroy_pipeline_layout &&
           g_up.fn.create_compute_pipelines && g_up.fn.destroy_pipeline &&
           g_up.fn.create_descriptor_pool && g_up.fn.destroy_descriptor_pool &&
           g_up.fn.allocate_descriptor_sets && g_up.fn.update_descriptor_sets;
}

/* --------------------------------------------------------------- the methods */

/* The methods that upscale with EASU and then refine at destination resolution.
 * They all run scale.comp with mode 4 first. */
static int mode_is_refined(uint32_t m) {
    return m == LSL_MODE_FSR1RCAS || m == LSL_MODE_NIS || m == LSL_MODE_CNN;
}

/* LSL_LAYER_MODE=auto.  The layer makes this choice once per swapchain, because
 * a per-frame one would flicker as the picture changed.  The old policy used
 * geometry alone.  That was unsafe on llvmpipe/CPU Vulkan and needlessly
 * conservative on a real discrete GPU, so use the physical-device properties
 * as well.  `cnn` is an AI-style learned restoration pass implemented as a
 * Vulkan compute shader; it is not DLSS and does not require a vendor SDK.
 *
 * There is no free lunch to hand out, so the rule follows the measurements
 * (1600x1000 -> 2560x1600; dispatch cost per frame, and PSNR / edge energy
 * against the original in `--render` quality mode, where the ground truth's
 * edge energy is 3.69):
 *
 *   method     cost      PSNR      edge
 *   fsr1rcas   1.97 ms   35.36 dB  4.23   the historical default; RCAS at
 *                                         strength 0 overshoots the edge
 *                                         energy of the original
 *   nis        2.87 ms   36.05 dB  3.01   costs 1.5x, scores a bit better,
 *                                         sharpens much more gently
 *   lanczos3   3.39 ms   36.85 dB  1.94   best PSNR, no sharpening at all
 *   cnn       20.03 ms   36.15 dB  2.33   four float convolutions, ~5 ns a
 *                                         destination pixel
 *
 * Hence cnn is the one that has to be rationed, and by output size rather than
 * ratio: it is the only method that reconstructs rather than resamples, and
 * 1280x720 -- about 1 Mpx, roughly 5 ms -- is where it stops fitting a frame
 * (an application rendering 640x360 for a 720p window can afford it).  Above
 * that, a big ratio means the application really did throw detail away, and
 * the milder sharpening is a better match than RCAS's maximum lobe: that is
 * what nis is for.  Everything else stays on the historical default.
 */
#define LSL_AUTO_CNN_MAX_PIXELS_DISCRETE (1280u * 720u)
#define LSL_AUTO_CNN_MAX_PIXELS_INTEGRATED (960u * 540u)

static uint32_t lsl_auto_pick(VkPhysicalDevice pd, uint32_t app_w, uint32_t app_h,
                              uint32_t real_w, uint32_t real_h) {
    const char *ai = lsl_env_str("LSL_LAYER_AI");
    if (ai && (!strcmp(ai, "off") || !strcmp(ai, "0") || !strcmp(ai, "false")))
        return LSL_MODE_FSR1RCAS;

    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    PFN_vkGetPhysicalDeviceProperties get_props = NULL;
    if (pd != VK_NULL_HANDLE)
        get_props = (PFN_vkGetPhysicalDeviceProperties)
            lsl_pd_proc(pd, "vkGetPhysicalDeviceProperties");
    if (pd == VK_NULL_HANDLE || !get_props)
        props.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    else
        get_props(pd, &props);

    const int ai_auto = !ai || !*ai || !strcmp(ai, "auto");
    const int force_cnn = ai && (!strcmp(ai, "cnn") || !strcmp(ai, "force"));
    const int cpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const uint64_t pixels = (uint64_t)real_w * real_h;
    const uint64_t cnn_limit = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
        ? LSL_AUTO_CNN_MAX_PIXELS_INTEGRATED
        : LSL_AUTO_CNN_MAX_PIXELS_DISCRETE;

    /* Explicit force is useful for testing and for users with a fast
     * integrated GPU.  Never send a CPU/software Vulkan device through the
     * CNN implicitly: it can turn a 5 ms pass into hundreds of milliseconds. */
    if (!cpu && (force_cnn || (ai_auto && pixels <= cnn_limit)))
        return LSL_MODE_CNN;
    const float rx = (float)real_w / (float)app_w;
    const float ry = (float)real_h / (float)app_h;
    const float ratio = rx > ry ? rx : ry;
    if (!cpu && ratio >= 1.5f) return LSL_MODE_NIS;
    return LSL_MODE_FSR1RCAS;
}

/* shaders/cnn.comp hardcodes the 3x3 tap order; the generated weight table
 * records the order the generator validated it against.  If they disagreed the
 * GPU and the CPU reference would multiply the same weight by different
 * neighbours.  tools/anime4k_weights.py already refuses any other order, so
 * this is the cheap check that also keeps the table referenced. */
static int anime4k_layout_ok(void) {
    static const int want[9][2] = {{-1, -1}, {-1, 0}, {-1, 1},
                                   {0, -1},  {0, 0},  {0, 1},
                                   {1, -1},  {1, 0},  {1, 1}};
    for (int i = 0; i < 9; ++i)
        if (lsl_anime4k_offsets[i][0] != want[i][0] ||
            lsl_anime4k_offsets[i][1] != want[i][1])
            return 0;
    return 1;
}

static uint32_t pick_queue_family(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    if (pd == VK_NULL_HANDLE) return UINT32_MAX;
    /* Through the instance that owns `pd`, not whichever was created last: the
     * surface belongs to that instance too. */
    PFN_vkGetPhysicalDeviceQueueFamilyProperties props_fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            lsl_pd_proc(pd, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!props_fn) return UINT32_MAX;

    uint32_t n = 0;
    props_fn(pd, &n, NULL);
    if (n == 0) return UINT32_MAX;
    VkQueueFamilyProperties *props = malloc(n * sizeof(*props));
    if (!props) return UINT32_MAX;
    props_fn(pd, &n, props);

    PFN_vkGetPhysicalDeviceSurfaceSupportKHR support_fn = NULL;
    if (surface != VK_NULL_HANDLE)
        support_fn = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)
                         lsl_pd_proc(pd, "vkGetPhysicalDeviceSurfaceSupportKHR");

    uint32_t chosen = UINT32_MAX, fallback = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (!(props[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (fallback == UINT32_MAX) fallback = i;
        if (support_fn) {
            VkBool32 sup = VK_FALSE;
            if (support_fn(pd, i, surface, &sup) == VK_SUCCESS && sup) {
                chosen = i;
                break;
            }
        }
    }
    free(props);
    return (chosen != UINT32_MAX) ? chosen : fallback;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags want, VkBuffer *buf,
                       VkDeviceMemory *mem) {
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_up.fn.create_buffer(up_dev(), &bci, NULL, buf) != VK_SUCCESS) return 0;

    VkMemoryRequirements mr;
    g_up.fn.get_buffer_memory_requirements(up_dev(), *buf, &mr);

    uint32_t type;
    if (!lsl_pick_memory_type(mr.memoryTypeBits, want, 0, &type) &&
        !lsl_pick_memory_type(mr.memoryTypeBits, 0, 0, &type)) {
        g_up.fn.destroy_buffer(up_dev(), *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    if (g_up.fn.allocate_memory(up_dev(), &mai, NULL, mem) != VK_SUCCESS) {
        g_up.fn.destroy_buffer(up_dev(), *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    if (g_up.fn.bind_buffer_memory(up_dev(), *buf, *mem, 0) != VK_SUCCESS) {
        g_up.fn.free_memory(up_dev(), *mem, NULL);
        g_up.fn.destroy_buffer(up_dev(), *buf, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static void free_buffer(VkBuffer buf, VkDeviceMemory mem) {
    VkDevice dev = up_dev();
    if (buf) g_up.fn.destroy_buffer(dev, buf, NULL);
    if (mem) g_up.fn.free_memory(dev, mem, NULL);
}

static VkPipeline make_pipeline(const uint32_t *code, size_t words,
                                VkPipelineLayout layout) {
    VkShaderModuleCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words * sizeof(uint32_t);
    sci.pCode = code;

    VkShaderModule mod = VK_NULL_HANDLE;
    if (g_up.fn.create_shader_module(up_dev(), &sci, NULL, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stage;
    memset(&stage, 0, sizeof(stage));
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkComputePipelineCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage = stage;
    pci.layout = layout;

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = g_up.fn.create_compute_pipelines(up_dev(), VK_NULL_HANDLE, 1, &pci, NULL, &pipe);
    g_up.fn.destroy_shader_module(up_dev(), mod, NULL);
    return (r == VK_SUCCESS) ? pipe : VK_NULL_HANDLE;
}

static void write_sets(void) {
    const int with_cnn = (g_up.mode == LSL_MODE_CNN);
    const uint32_t n_bind = with_cnn ? 5u : 2u;

    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        VkDescriptorBufferInfo bi[5];
        VkWriteDescriptorSet w[5];
        memset(w, 0, sizeof(w));
        for (uint32_t k = 0; k < n_bind; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &bi[k];
            bi[k].offset = 0;
            bi[k].range = VK_WHOLE_SIZE;
        }

        bi[0].buffer = s->in;
        bi[1].buffer = s->mid;
        w[0].dstSet = s->set_scale;
        w[1].dstSet = s->set_scale;
        g_up.fn.update_descriptor_sets(up_dev(), 2, w, 0, NULL);

        bi[0].buffer = s->mid;
        bi[1].buffer = s->in;
        w[0].dstSet = s->set_refine;
        w[1].dstSet = s->set_refine;
        g_up.fn.update_descriptor_sets(up_dev(), 2, w, 0, NULL);

        if (with_cnn) {
            /* mid, the two activations, the shared weight table, and the
             * output buffer shaders/cnn.comp's writeImage lands in. */
            bi[0].buffer = s->mid;
            bi[1].buffer = s->cnn_a;
            bi[2].buffer = s->cnn_b;
            bi[3].buffer = g_up.wts;
            bi[4].buffer = s->in;
            for (uint32_t k = 0; k < 5; ++k) w[k].dstSet = s->set_cnn;
            g_up.fn.update_descriptor_sets(up_dev(), 5, w, 0, NULL);
        }
    }
}

/* Nothing the engine owns may be freed while the queue is still reading it.
 *
 * A resize is the case that matters: the application creates the new swapchain
 * without waiting for its own queue, so a dispatch submitted for the previous
 * frame -- reading exactly the buffers a rebuild would free -- can still be
 * running.  Two slots bound the wait to the frames already handed to the queue,
 * and this happens once per resize, never per frame. */
static void up_wait_slots(void) {
    if (!g_up.fn.wait_for_fences) return;
    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        if (!s->submitted || !s->fence) continue;
        if (g_up.fn.wait_for_fences(up_dev(), 1, &s->fence, VK_TRUE,
                                    2000000000ull) != VK_SUCCESS)
            lsl_log("scale: the queue did not drain before the upscaler was rebuilt");
        s->submitted = 0;
    }
}

/* The only extent-sized objects: each slot's planes, the CNN activations when
 * the method uses them, and the staging buffer the dump reads through.
 * Everything else -- pipelines, descriptor sets, the command pool -- is
 * extent-independent, which is what makes a resize cheap.  The caller calls
 * write_sets() once the descriptor sets exist. */
static void up_free_planes(void) {
    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        free_buffer(s->in, s->in_mem);
        free_buffer(s->mid, s->mid_mem);
        free_buffer(s->cnn_a, s->cnn_a_mem);
        free_buffer(s->cnn_b, s->cnn_b_mem);
        s->in = VK_NULL_HANDLE;
        s->in_mem = VK_NULL_HANDLE;
        s->mid = VK_NULL_HANDLE;
        s->mid_mem = VK_NULL_HANDLE;
        s->cnn_a = VK_NULL_HANDLE;
        s->cnn_a_mem = VK_NULL_HANDLE;
        s->cnn_b = VK_NULL_HANDLE;
        s->cnn_b_mem = VK_NULL_HANDLE;
    }
    free_buffer(g_up.stage, g_up.stage_mem);
    g_up.stage = VK_NULL_HANDLE;
    g_up.stage_mem = VK_NULL_HANDLE;
}

static int up_make_planes(void) {
    const int with_cnn = (g_up.mode == LSL_MODE_CNN);
    /* The CNN's activations are floats -- two RGBA planes, 16 bytes a pixel --
     * whatever the packed end of the chain is. */
    const VkDeviceSize cnn_bytes =
        (VkDeviceSize)g_up.shape.real_w * g_up.shape.real_h * 16u;

    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        /* `in` is copied into (TRANSFER_DST), copied out of for the dump and the
         * copy back (TRANSFER_SRC), and read and written by the shaders.  `mid`
         * is written by the first pass and read by the refiner and the dump. */
        if (!make_buffer(g_up.plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s->in, &s->in_mem))
            return 0;
        if (!make_buffer(g_up.plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s->mid, &s->mid_mem))
            return 0;
        if (with_cnn) {
            if (!make_buffer(cnn_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &s->cnn_a, &s->cnn_a_mem))
                return 0;
            if (!make_buffer(cnn_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &s->cnn_b, &s->cnn_b_mem))
                return 0;
        }
    }
    if (!make_buffer(g_up.stage_easu_off + g_up.plane_bytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &g_up.stage, &g_up.stage_mem))
        return 0;
    return 1;
}

/* The buffers of a given extent plus the descriptor writes that name them.
 * Separate from up_make_planes() only because the rebuild path runs with the
 * descriptor sets already in place, while the first build creates them first and
 * writes them here. */
static int up_make_buffers(void) {
    if (!up_make_planes()) return 0;
    write_sets();
    return 1;
}

/* Wait for whatever the queue has not finished yet.
 *
 * The application must wait for its own submission before it destroys a
 * swapchain, but the dispatch this layer submits at present time does not
 * belong to that submission: it is queued after it, so it can still be reading
 * the images of the swapchain being destroyed.  Called from
 * vkDestroySwapchainKHR, and only for a swapchain this layer upscaled -- a
 * resize rarely needs it (the rebuild already drained), but an application that
 * destroys without recreating first does. */
void lsl_upscale_drain(void) { up_wait_slots(); }

void lsl_upscale_end(void) {
    /* The engine's objects may belong to a device that is not the newest one
     * (see Upscale.dev), so destroy them with their own device handle. */
    VkDevice dev = g_up.dev ? g_up.dev : up_dev();
    if (!dev) {
        memset(&g_up, 0, sizeof(g_up));
        return;
    }
    up_wait_slots();
    up_free_planes();
    free_buffer(g_up.wts, g_up.wts_mem);
    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        if (s->fence && g_up.fn.destroy_fence) g_up.fn.destroy_fence(dev, s->fence, NULL);
    }
    if (g_up.dpool && g_up.fn.destroy_descriptor_pool)
        g_up.fn.destroy_descriptor_pool(dev, g_up.dpool, NULL);
    if (g_up.pool && g_up.fn.destroy_command_pool)
        g_up.fn.destroy_command_pool(dev, g_up.pool, NULL);
    if (g_up.pipe_scale && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_scale, NULL);
    if (g_up.pipe_rcas && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_rcas, NULL);
    if (g_up.pipe_nis && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_nis, NULL);
    if (g_up.pipe_cnn && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_cnn, NULL);
    if (g_up.playout && g_up.fn.destroy_pipeline_layout)
        g_up.fn.destroy_pipeline_layout(dev, g_up.playout, NULL);
    if (g_up.playout_cnn && g_up.fn.destroy_pipeline_layout)
        g_up.fn.destroy_pipeline_layout(dev, g_up.playout_cnn, NULL);
    if (g_up.dsl && g_up.fn.destroy_descriptor_set_layout)
        g_up.fn.destroy_descriptor_set_layout(dev, g_up.dsl, NULL);
    if (g_up.dsl_cnn && g_up.fn.destroy_descriptor_set_layout)
        g_up.fn.destroy_descriptor_set_layout(dev, g_up.dsl_cnn, NULL);
    memset(&g_up, 0, sizeof(g_up));
}

static int same_shape(const LslUpscaleShape *a, const LslUpscaleShape *b) {
    return a->format == b->format && a->real_w == b->real_w &&
           a->real_h == b->real_h && a->app_w == b->app_w &&
           a->app_h == b->app_h && a->mode == b->mode;
}

/* The engine's command pool and its barriers are tied to one queue family, so a
 * swapchain whose surface presents from another family needs a full rebuild
 * rather than the extent-only path. */
static int surface_family_matches(VkDevice dev, VkSurfaceKHR surface) {
    if (surface == VK_NULL_HANDLE) return 1;
    VkPhysicalDevice pd = lsl_pd_of_dev(dev);
    if (pd == VK_NULL_HANDLE) pd = g_physdev;
    if (pd == VK_NULL_HANDLE) return 0;
    return pick_queue_family(pd, surface) == g_up.family;
}

VkDevice lsl_upscale_device(void) { return g_up.valid ? g_up.dev : VK_NULL_HANDLE; }

int lsl_upscale_begin(const LslUpscaleShape *shape, VkSurfaceKHR surface, VkDevice dev) {
    if (!shape || dev == VK_NULL_HANDLE || shape->app_w == 0 || shape->app_h == 0) return 0;
    /* Nothing to do when the application already renders at full size. */
    if (shape->real_w <= shape->app_w && shape->real_h <= shape->app_h) return 0;
    if (!lsl_format_supported(shape->format)) {
        lsl_log("scale: swapchain format %d (%s) has no upscale path; "
                "the application will render at full size",
                (int)shape->format, format_name(shape->format));
        return 0;
    }

    VkPhysicalDevice pd = lsl_pd_of_dev(dev);
    if (pd == VK_NULL_HANDLE) pd = g_physdev;

    /* Resolve "auto" here rather than per frame, so the choice is a property of
     * the swapchain.  See lsl_auto_pick(). */
    const int declared_auto = (shape->mode == LSL_MODE_AUTO);
    const uint32_t mode = declared_auto
        ? lsl_auto_pick(pd, shape->app_w, shape->app_h, shape->real_w, shape->real_h)
        : shape->mode;
    /* The shape carries the declared mode, so an auto engine compares equal to
     * itself on the next frame of the same swapchain. */
    if (g_up.valid && g_up.dev == dev && same_shape(&g_up.shape, shape)) return 1;

    /* Same device, same pixel format, and the same present queue family: a
     * resize, and nothing else.  The pipelines, descriptor sets and command
     * buffers do not depend on the extent, so keep them and rebuild only the
     * buffers.  A window being dragged then costs one buffer set per resize
     * instead of a whole pipeline build -- and, more importantly, the rebuild
     * waits for the frames still in flight on the queue before it frees
     * anything, which a resize does not otherwise make the application do. */
    if (g_up.valid && g_up.dev == dev && g_up.mode == mode &&
        g_up.is16 == (lsl_format_is_16f(shape->format) != 0) &&
        g_up.texel == lsl_format_texel_bytes(shape->format) &&
        surface_family_matches(dev, surface)) {
        up_wait_slots();
        g_up.shape = *shape;
        g_up.plane_bytes = (VkDeviceSize)shape->real_w * shape->real_h * g_up.texel;
        g_up.stage_out_off = (VkDeviceSize)shape->app_w * shape->app_h * g_up.texel;
        g_up.stage_easu_off = g_up.stage_out_off + g_up.plane_bytes;
        g_up.next_slot = 0;
        up_free_planes();
        if (!up_make_buffers()) {
            lsl_log("scale: could not size the upscaler's buffers for %ux%u -> %ux%u; "
                    "the application will render at full size",
                    shape->app_w, shape->app_h, shape->real_w, shape->real_h);
            lsl_upscale_end();
            return 0;
        }
        lsl_log("scale: resized to %ux%u -> %ux%u (pipelines kept)",
                shape->app_w, shape->app_h, shape->real_w, shape->real_h);
        return 1;
    }

    /* A different device (or a different pixel format) means the old engine --
     * pipelines, buffers, command pools -- cannot be reused here.  Drop it,
     * with its own device handle, and build again.  A process with more than one
     * live device gets one engine at a time, which is the single-device limit
     * noted in lsl_layer.c. */
    lsl_upscale_end();

    /* `dev`'s own chain, not whichever device was created last. */
    lsl_resolve_device_fns(dev, &g_up.fn);
    if (!up_have_fns()) {
        lsl_log("scale: not every entry point the upscaler needs was resolved");
        memset(&g_up, 0, sizeof(g_up));
        return 0;
    }

    g_up.shape = *shape;
    g_up.dev = dev;
    g_up.mode = mode;
    g_up.declared_auto = declared_auto;
    g_up.is16 = lsl_format_is_16f(shape->format);
    g_up.texel = lsl_format_texel_bytes(shape->format);
    g_up.plane_bytes = (VkDeviceSize)shape->real_w * shape->real_h * g_up.texel;
    g_up.stage_out_off = (VkDeviceSize)shape->app_w * shape->app_h * g_up.texel;
    g_up.stage_easu_off = g_up.stage_out_off + g_up.plane_bytes;
    g_up.next_slot = 0;

    g_up.family = pick_queue_family(pd, surface);
    if (g_up.family == UINT32_MAX) {
        lsl_log("scale: no graphics+compute queue family to run the upscale on");
        memset(&g_up, 0, sizeof(g_up));
        return 0;
    }

    const char *why = NULL;
#define UP_FAIL(msg) do { why = (msg); goto fail; } while (0)
#define UP_CHECK(call, msg) do {                     \
        VkResult _r = (call);                        \
        if (_r != VK_SUCCESS) {                      \
            lsl_log("scale: %s -> %d", (msg), (int)_r); \
            why = (msg);                             \
            goto fail;                               \
        }                                            \
    } while (0)

    const int with_rcas = (mode == LSL_MODE_FSR1RCAS);
    const int with_nis  = (mode == LSL_MODE_NIS);
    const int with_cnn  = (mode == LSL_MODE_CNN);

    /* scale.comp, rcas.comp and nis.comp all take (src, dst); the CNN takes
     * five buffers and gets its own layout below. */
    VkDescriptorSetLayoutBinding bind[5];
    memset(bind, 0, sizeof(bind));
    for (uint32_t k = 0; k < 5; ++k) {
        bind[k].binding = k;
        bind[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bind[k].descriptorCount = 1;
        bind[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci;
    memset(&dlci, 0, sizeof(dlci));
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 2;
    dlci.pBindings = bind;
    UP_CHECK(g_up.fn.create_descriptor_set_layout(up_dev(), &dlci, NULL, &g_up.dsl),
             "create_descriptor_set_layout");

    VkPushConstantRange pcr;
    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = (uint32_t)sizeof(PushConst);

    VkPushConstantRange pcr_cnn;
    memset(&pcr_cnn, 0, sizeof(pcr_cnn));
    pcr_cnn.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr_cnn.size = (uint32_t)sizeof(CnnPushConst);

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_up.dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    UP_CHECK(g_up.fn.create_pipeline_layout(up_dev(), &plci, NULL, &g_up.playout),
             "create_pipeline_layout");

    /* scale.comp is every method's first pass, so it is always built.  The
     * refiners are built only for the method that will run them, which keeps
     * the default's cost exactly what it was before the methods existed. */
    if (g_up.is16)
        g_up.pipe_scale = make_pipeline(lsl_spv_scale16_comp, lsl_spv_scale16_comp_words, g_up.playout);
    else
        g_up.pipe_scale = make_pipeline(lsl_spv_scale_comp, lsl_spv_scale_comp_words, g_up.playout);
    if (!g_up.pipe_scale) UP_FAIL("the scale compute pipeline");

    if (with_rcas) {
        if (g_up.is16)
            g_up.pipe_rcas = make_pipeline(lsl_spv_rcas16_comp, lsl_spv_rcas16_comp_words, g_up.playout);
        else
            g_up.pipe_rcas = make_pipeline(lsl_spv_rcas_comp, lsl_spv_rcas_comp_words, g_up.playout);
        if (!g_up.pipe_rcas) UP_FAIL("the RCAS compute pipeline");
    }

    if (with_nis) {
        if (g_up.is16)
            g_up.pipe_nis = make_pipeline(lsl_spv_nis16_comp, lsl_spv_nis16_comp_words, g_up.playout);
        else
            g_up.pipe_nis = make_pipeline(lsl_spv_nis_comp, lsl_spv_nis_comp_words, g_up.playout);
        if (!g_up.pipe_nis) UP_FAIL("the NIS compute pipeline");
    }

    if (with_cnn) {
        if (!anime4k_layout_ok()) UP_FAIL("the Anime4K tap order");

        dlci.bindingCount = 5;
        UP_CHECK(g_up.fn.create_descriptor_set_layout(up_dev(), &dlci, NULL, &g_up.dsl_cnn),
                 "create the CNN descriptor set layout");

        plci.pSetLayouts = &g_up.dsl_cnn;
        plci.pPushConstantRanges = &pcr_cnn;
        UP_CHECK(g_up.fn.create_pipeline_layout(up_dev(), &plci, NULL, &g_up.playout_cnn),
                 "create the CNN pipeline layout");

        if (g_up.is16)
            g_up.pipe_cnn = make_pipeline(lsl_spv_cnn16_comp, lsl_spv_cnn16_comp_words, g_up.playout_cnn);
        else
            g_up.pipe_cnn = make_pipeline(lsl_spv_cnn_comp, lsl_spv_cnn_comp_words, g_up.playout_cnn);
        if (!g_up.pipe_cnn) UP_FAIL("the CNN compute pipeline");

        /* The weight table is 4 KB, read-only and the same for every frame and
         * every extent, so it lives in host memory rather than being staged to
         * the device: it stays resident in the GPU's cache, and the engine does
         * not need the queue this early to upload it. */
        if (!make_buffer((VkDeviceSize)sizeof(lsl_anime4k_weights),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &g_up.wts, &g_up.wts_mem))
            UP_FAIL("the CNN weight buffer");
        void *wmap = NULL;
        UP_CHECK(g_up.fn.map_memory(up_dev(), g_up.wts_mem, 0,
                                    (VkDeviceSize)sizeof(lsl_anime4k_weights), 0, &wmap),
                 "mapping the CNN weights");
        memcpy(wmap, lsl_anime4k_weights, sizeof(lsl_anime4k_weights));
        g_up.fn.unmap_memory(up_dev(), g_up.wts_mem);
    }

    VkDescriptorPoolSize dps;
    memset(&dps, 0, sizeof(dps));
    dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    /* Two sets per slot -- scale and refine -- plus the CNN set, which names
     * five buffers rather than two, when the method uses it. */
    dps.descriptorCount = (2u + 2u + (with_cnn ? 5u : 0u)) * UP_SLOTS;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = (2u + (with_cnn ? 1u : 0u)) * UP_SLOTS;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    UP_CHECK(g_up.fn.create_descriptor_pool(up_dev(), &dpci, NULL, &g_up.dpool),
             "create_descriptor_pool");

    VkDescriptorSetLayout layouts[UP_SLOTS];
    for (int i = 0; i < UP_SLOTS; ++i) layouts[i] = g_up.dsl;
    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_up.dpool;
    dsai.descriptorSetCount = UP_SLOTS;
    dsai.pSetLayouts = layouts;

    VkDescriptorSet sets[UP_SLOTS];
    UP_CHECK(g_up.fn.allocate_descriptor_sets(up_dev(), &dsai, sets), "allocate scale sets");
    for (int i = 0; i < UP_SLOTS; ++i) g_up.slot[i].set_scale = sets[i];
    UP_CHECK(g_up.fn.allocate_descriptor_sets(up_dev(), &dsai, sets), "allocate refine sets");
    for (int i = 0; i < UP_SLOTS; ++i) g_up.slot[i].set_refine = sets[i];
    if (with_cnn) {
        for (int i = 0; i < UP_SLOTS; ++i) layouts[i] = g_up.dsl_cnn;
        UP_CHECK(g_up.fn.allocate_descriptor_sets(up_dev(), &dsai, sets), "allocate CNN sets");
        for (int i = 0; i < UP_SLOTS; ++i) g_up.slot[i].set_cnn = sets[i];
    }

    /* The descriptor sets have to exist before the buffers they name, and the
     * extent-sized buffers before the command buffers that copy into them. */
    if (!up_make_buffers()) UP_FAIL("the extent-sized buffers");

    for (int i = 0; i < UP_SLOTS; ++i) {
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        UP_CHECK(g_up.fn.create_fence(up_dev(), &fci, NULL, &g_up.slot[i].fence),
                 "create_fence");
    }

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_up.family;
    UP_CHECK(g_up.fn.create_command_pool(up_dev(), &cpci, NULL, &g_up.pool),
             "create_command_pool");

    for (int i = 0; i < UP_SLOTS; ++i) {
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_up.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        UP_CHECK(g_up.fn.allocate_command_buffers(up_dev(), &cbai, &g_up.slot[i].cmd),
                 "allocate_command_buffers");
        lsl_stamp_command_buffers(up_dev(), 1, &g_up.slot[i].cmd);
    }

    /* write_sets() already ran, for the new plane buffers. */
    g_up.valid = 1;
    /* One line, driven off by tools/upscale-check.sh: it runs the layer for a
     * method, greps this line to learn what was built, and compares the dumped
     * frame against `lsl-upscale --modes <mode>`.  The mode really running is
     * therefore on the line, and an LSL_LAYER_MODE=auto build says which
     * method it resolved to. */
    lsl_log("scale: %ux%u -> %ux%u via %s, %s pixels, queue family %u, "
            "mode %s%s",
            shape->app_w, shape->app_h, shape->real_w, shape->real_h,
            lsl_mode_build_text(mode), g_up.is16 ? "half-float" : "RGBA8",
            g_up.family, lsl_mode_name(mode), declared_auto ? " (auto)" : "");
#undef UP_CHECK
#undef UP_FAIL
    return 1;

fail:
    lsl_log("scale: could not build the upscale pipeline (%s); the application "
            "will render at full size", why ? why : "unknown");
    lsl_upscale_end();
    return 0;
}

uint32_t lsl_upscale_family(void) { return g_up.valid ? g_up.family : UINT32_MAX; }

/* ------------------------------------------------------------- command record */

static void up_buf_barrier(VkCommandBuffer cmd, VkBuffer buf,
                           VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags src, VkAccessFlags dst) {
    VkBufferMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    g_up.fn.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, NULL, 1, &b, 0, NULL);
}

static void up_img_barrier(VkCommandBuffer cmd, VkImage img,
                           VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    g_up.fn.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void up_record(UpSlot *s, VkImage image, int with_dump) {
    const uint32_t W = g_up.shape.app_w, H = g_up.shape.app_h;
    const uint32_t N = g_up.shape.real_w, M = g_up.shape.real_h;
    const VkDeviceSize texel = g_up.texel;
    VkCommandBuffer cmd = s->cmd;

    g_up.fn.reset_command_buffer(cmd, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_up.fn.begin_command_buffer(cmd, &bi);

    /* The application finished its render pass by moving the image to
     * PRESENT_SRC_KHR, which is where we pick it up. */
    up_img_barrier(cmd, image,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    /* Only the top-left W x H corner holds anything: that is the region the
     * application believes is the whole swapchain.  Tightly packed, so the
     * shaders can treat it as a W x H image. */
    VkBufferImageCopy src;
    memset(&src, 0, sizeof(src));
    src.bufferOffset = 0;
    src.bufferRowLength = W;
    src.bufferImageHeight = H;
    src.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src.imageSubresource.layerCount = 1;
    src.imageExtent.width = W;
    src.imageExtent.height = H;
    src.imageExtent.depth = 1;
    g_up.fn.cmd_copy_image_to_buffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  s->in, 1, &src);

    up_buf_barrier(cmd, s->in, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    up_img_barrier(cmd, image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    if (with_dump) {
        VkBufferCopy c;
        memset(&c, 0, sizeof(c));
        c.srcOffset = 0;
        c.dstOffset = 0;
        c.size = (VkDeviceSize)W * H * texel;
        g_up.fn.cmd_copy_buffer(cmd, s->in, g_up.stage, 1, &c);
    }

    PushConst pc;
    memset(&pc, 0, sizeof(pc));
    /* Every method's first pass.  The refined methods run EASU here and a
     * second pass at destination resolution below; a pure spatial method is
     * this single dispatch, and its result lands in `mid`. */
    const int refined = mode_is_refined(g_up.mode);
    pc.srcW = W; pc.srcH = H; pc.dstW = N; pc.dstH = M;
    pc.mode = refined ? (uint32_t)LSL_MODE_FSR1 : g_up.mode;
    pc.sharpness = g_up.shape.sharpness;
    g_up.fn.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.pipe_scale);
    g_up.fn.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.playout,
                                  0, 1, &s->set_scale, 0, NULL);
    g_up.fn.cmd_push_constants(cmd, g_up.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    g_up.fn.cmd_dispatch(cmd, UP_GROUPS(N), UP_GROUPS(M), 1);

    if (refined) {
        /* The next pass reads what the first one just wrote to `mid`, and it
         * overwrites `in` -- which the first pass read.  One barrier with two
         * buffer entries expresses both dependencies. */
        VkBufferMemoryBarrier b[2];
        memset(b, 0, sizeof(b));
        b[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[0].buffer = s->mid;
        b[0].offset = 0;
        b[0].size = VK_WHOLE_SIZE;
        b[1] = b[0];
        b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b[1].buffer = s->in;
        g_up.fn.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                                  2, b, 0, NULL);
    }

    if (g_up.mode == LSL_MODE_FSR1RCAS || g_up.mode == LSL_MODE_NIS) {
        /* Both refiners are a 1:1 pass at destination resolution over `mid`,
         * writing the finished frame into `in`.  RCAS uses `sharpness`; NIS
         * ignores it and applies its own fixed tuning. */
        pc.srcW = N; pc.srcH = M; pc.dstW = N; pc.dstH = M; pc.mode = 0;
        if (g_up.mode != LSL_MODE_FSR1RCAS) pc.sharpness = 0.0f;
        g_up.fn.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  g_up.mode == LSL_MODE_FSR1RCAS ? g_up.pipe_rcas
                                                                 : g_up.pipe_nis);
        g_up.fn.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.playout,
                                      0, 1, &s->set_refine, 0, NULL);
        g_up.fn.cmd_push_constants(cmd, g_up.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        g_up.fn.cmd_dispatch(cmd, UP_GROUPS(N), UP_GROUPS(M), 1);
    } else if (g_up.mode == LSL_MODE_CNN) {
        /* Four convolutions over two ping-ponged float planes.  The layer table
         * -- generated alongside the weights -- says where each layer's
         * matrices are and whether it splits, so the shader and the CPU
         * reference read the same numbers; only the buffer roles differ per
         * layer, and those are the flags below. */
        for (uint32_t L = 0; L < (uint32_t)LSL_ANIME4K_LAYERS; ++L) {
            if (L) {
                /* The previous layer wrote one of the two planes; this one
                 * reads it.  (L - 1) even means layer L-1 wrote A. */
                up_buf_barrier(cmd, ((L - 1u) & 1u) ? s->cnn_b : s->cnn_a,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            }
            uint32_t flags = 0;
            if (lsl_anime4k_layers[L].split)    flags |= LSL_CNN_SPLIT;
            if (lsl_anime4k_layers[L].add_base) flags |= LSL_CNN_ADD_BASE;
            if (L == 0) flags |= LSL_CNN_READ_BASE;
            if (L == 1) flags |= LSL_CNN_DST_B;
            if (L == 2) flags |= LSL_CNN_SRC_B;
            if (L == (uint32_t)LSL_ANIME4K_LAYERS - 1u) flags |= LSL_CNN_WRITE_IMAGE;

            CnnPushConst cp;
            cp.W = N;
            cp.H = M;
            cp.woff = (uint32_t)lsl_anime4k_layers[L].woff;
            cp.boff = (uint32_t)lsl_anime4k_layers[L].boff;
            cp.flags = flags;
            g_up.fn.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.pipe_cnn);
            g_up.fn.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                          g_up.playout_cnn, 0, 1, &s->set_cnn, 0, NULL);
            g_up.fn.cmd_push_constants(cmd, g_up.playout_cnn, VK_SHADER_STAGE_COMPUTE_BIT,
                                       0, sizeof(cp), &cp);
            g_up.fn.cmd_dispatch(cmd, UP_GROUPS_CNN(N), UP_GROUPS_CNN(M), 1);
        }
    }

    /* Everything the shaders wrote has to be visible to the transfers below.
     * `mid` holds the base result in every method; a refined method leaves the
     * finished frame in `in`, a single-dispatch method leaves it in `mid`. */
    up_buf_barrier(cmd, s->mid, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBuffer outbuf = refined ? s->in : s->mid;
    if (refined)
        up_buf_barrier(cmd, s->in, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    if (with_dump) {
        /* `mid` holds the base result and `outbuf` the finished frame, so
         * tools/upscale-check.sh can tell a base disagreement from a refiner
         * one.  A single-dispatch method leaves both in the same buffer. */
        VkBufferCopy c;
        memset(&c, 0, sizeof(c));
        c.srcOffset = 0;
        c.dstOffset = g_up.stage_easu_off;
        c.size = (VkDeviceSize)N * M * texel;
        g_up.fn.cmd_copy_buffer(cmd, s->mid, g_up.stage, 1, &c);

        memset(&c, 0, sizeof(c));
        c.srcOffset = 0;
        c.dstOffset = g_up.stage_out_off;
        c.size = (VkDeviceSize)N * M * texel;
        g_up.fn.cmd_copy_buffer(cmd, outbuf, g_up.stage, 1, &c);
    }

    VkBufferImageCopy dst;
    memset(&dst, 0, sizeof(dst));
    dst.bufferOffset = 0;
    dst.bufferRowLength = N;
    dst.bufferImageHeight = M;
    dst.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst.imageSubresource.layerCount = 1;
    dst.imageExtent.width = N;
    dst.imageExtent.height = M;
    dst.imageExtent.depth = 1;
    g_up.fn.cmd_copy_buffer_to_image(cmd, outbuf, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dst);

    up_img_barrier(cmd, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0);

    g_up.fn.end_command_buffer(cmd);
}

/* ------------------------------------------------------------------- dumping */

/* Verification lives here: the frame the application rendered, and the frame we
 * upscaled it into, written as PPM so `lsl-upscale` can be pointed at the first
 * and the two results compared.  Channel order is left as the swapchain had it,
 * which is consistent between the two files, so the comparison is still exact
 * for the RGBA8 case. */
static uint8_t to_u8(float v) {
    float x = v * 255.0f + 0.5f;
    if (x < 0.0f) x = 0.0f;
    if (x > 255.0f) x = 255.0f;
    return (uint8_t)x;
}

static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) & 1u;
    const uint32_t exp = (uint32_t)(h >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)h & 0x3ffu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            uint32_t e = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                e--;
            }
            mant &= 0x3ffu;
            f = (sign << 31) | (e << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000u | (mant << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

static uint16_t read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write_ppm(const char *path, const uint8_t *data, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        lsl_log("scale: cannot write %s", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);

    uint8_t *row = malloc((size_t)w * 3u);
    if (!row) {
        fclose(f);
        return;
    }
    const int swap_rb = lsl_format_is_bgra(g_up.shape.format);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const VkDeviceSize i = (VkDeviceSize)y * w + x;
            float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (g_up.is16) {
                const uint8_t *p = data + i * 8u;
                c[0] = half_to_float(read_u16(p + 0));
                c[1] = half_to_float(read_u16(p + 2));
                c[2] = half_to_float(read_u16(p + 4));
            } else {
                const uint8_t *p = data + i * 4u;
                c[0] = (float)p[0] / 255.0f;
                c[1] = (float)p[1] / 255.0f;
                c[2] = (float)p[2] / 255.0f;
            }
            row[x * 3 + 0] = to_u8(c[swap_rb ? 2 : 0]);
            row[x * 3 + 1] = to_u8(c[1]);
            row[x * 3 + 2] = to_u8(c[swap_rb ? 0 : 2]);
        }
        fwrite(row, 3, w, f);
    }
    free(row);
    fclose(f);
    lsl_log("scale: wrote %s (%ux%u)", path, w, h);
}

static void up_dump(const char *prefix) {
    void *p = NULL;
    if (g_up.fn.map_memory(up_dev(), g_up.stage_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        lsl_log("scale: could not map the readback buffer");
        return;
    }
    const uint8_t *base = (const uint8_t *)p;

    char path[1024];
    snprintf(path, sizeof(path), "%s.src.ppm", prefix);
    write_ppm(path, base, g_up.shape.app_w, g_up.shape.app_h);
    snprintf(path, sizeof(path), "%s.out.ppm", prefix);
    write_ppm(path, base + g_up.stage_out_off, g_up.shape.real_w, g_up.shape.real_h);
    snprintf(path, sizeof(path), "%s.easu.ppm", prefix);
    write_ppm(path, base + g_up.stage_easu_off, g_up.shape.real_w, g_up.shape.real_h);

    g_up.fn.unmap_memory(up_dev(), g_up.stage_mem);
}

/* ------------------------------------------------------------------ the pass */

int lsl_upscale_run(VkQueue queue, uint32_t family, VkImage image,
                    const char *dump_prefix) {
    if (!g_up.valid) return 0;

    /* The image barriers above assume the queue is in the family the image's
     * queue ownership belongs to.  If the application presents from another
     * family the correct fix is a queue family ownership transfer, which this
     * prototype does not do -- so it declines rather than corrupts. */
    if (family != g_up.family) {
        static int warned;
        if (!warned) {
            warned = 1;
            lsl_log("scale: present queue is family %u but the upscale runs on "
                    "family %u; upscaling disabled for this application",
                    family, g_up.family);
        }
        return 0;
    }

    UpSlot *s = &g_up.slot[g_up.next_slot];
    g_up.next_slot = (g_up.next_slot + 1) % UP_SLOTS;

    if (s->submitted) {
        if (g_up.fn.wait_for_fences(up_dev(), 1, &s->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
            lsl_log("scale: slot fence timed out; skipping this frame");
            return 0;
        }
    }

    const int with_dump = dump_prefix && *dump_prefix && !g_dumped;

    up_record(s, image, with_dump);

    g_up.fn.reset_fences(up_dev(), 1, &s->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cmd;
    if (g_up.fn.queue_submit(queue, 1, &si, s->fence) != VK_SUCCESS) {
        lsl_log("scale: vkQueueSubmit failed; skipping this frame");
        return 0;
    }
    s->submitted = 1;

    if (with_dump) {
        if (g_up.fn.wait_for_fences(up_dev(), 1, &s->fence, VK_TRUE, 4000000000ull) == VK_SUCCESS) {
            up_dump(dump_prefix);
            g_dumped = 1;
        } else {
            lsl_log("scale: timed out waiting for the dump frame");
        }
    }
    return 1;
}
