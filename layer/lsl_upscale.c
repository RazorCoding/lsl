/*
 * lsl_upscale.c -- the layer's FSR1 upscaling engine.
 *
 * What this does, at present time, on the application's own queue:
 *
 *   swapchain image (N x M, but only the top-left W x H holds the app's frame)
 *     -> copy to buf_in
 *     -> EASU  (shaders/scale.comp, mode 4)          W x H -> N x M
 *     -> RCAS  (shaders/rcas.comp)                   N x M -> N x M
 *     -> copy back over the whole swapchain image
 *
 * Buffers rather than sampled images, because the Phase 1 shaders -- and the CPU
 * reference they are checked against -- work on tightly packed storage buffers.
 * Reusing the exact same shaders means the layer's output can be compared pixel
 * for pixel against `lsl-upscale` fed the same input, which is how this gets
 * verified without looking at the screen (see tools/upscale-check.sh).
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

/* Must match the push_constant block in shaders/scale.comp and shaders/rcas.comp. */
typedef struct {
    uint32_t srcW, srcH, dstW, dstH;
    uint32_t mode;      /* 4 = FSR1 EASU; ignored by rcas.comp */
    float sharpness;
    uint32_t pad0, pad1;
} PushConst;

/* Two in flight.  A slot's buffers and command buffer may not be touched again
 * until its fence signals, and the fence for an older slot is normally already
 * signalled, so reuse does not cost a stall. */
#define UP_SLOTS 2

/* shaders/scale.comp and shaders/rcas.comp both use 16x16 workgroups. */
#define UP_GROUPS(n) (((n) + 15u) / 16u)

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    int submitted;
    VkBuffer in, mid;          /* plane_bytes each */
    VkDeviceMemory in_mem, mid_mem;
    VkDescriptorSet set_easu;  /* in  -> mid */
    VkDescriptorSet set_rcas;  /* mid -> in  */
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
    uint32_t family;
    uint32_t texel;
    int is16;
    VkDeviceSize plane_bytes;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout playout;
    VkPipeline pipe_easu, pipe_rcas;
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
    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet w[2];
        memset(w, 0, sizeof(w));
        for (int k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstBinding = (uint32_t)k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &bi[k];
            bi[k].offset = 0;
            bi[k].range = VK_WHOLE_SIZE;
        }

        bi[0].buffer = s->in;
        bi[1].buffer = s->mid;
        w[0].dstSet = s->set_easu;
        w[1].dstSet = s->set_easu;
        g_up.fn.update_descriptor_sets(up_dev(), 2, w, 0, NULL);

        bi[0].buffer = s->mid;
        bi[1].buffer = s->in;
        w[0].dstSet = s->set_rcas;
        w[1].dstSet = s->set_rcas;
        g_up.fn.update_descriptor_sets(up_dev(), 2, w, 0, NULL);
    }
}

void lsl_upscale_end(void) {
    /* The engine's objects may belong to a device that is not the newest one
     * (see Upscale.dev), so destroy them with their own device handle. */
    VkDevice dev = g_up.dev ? g_up.dev : up_dev();
    if (!dev) {
        memset(&g_up, 0, sizeof(g_up));
        return;
    }
    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        free_buffer(s->in, s->in_mem);
        free_buffer(s->mid, s->mid_mem);
        if (s->fence && g_up.fn.destroy_fence) g_up.fn.destroy_fence(dev, s->fence, NULL);
    }
    free_buffer(g_up.stage, g_up.stage_mem);
    if (g_up.dpool && g_up.fn.destroy_descriptor_pool)
        g_up.fn.destroy_descriptor_pool(dev, g_up.dpool, NULL);
    if (g_up.pool && g_up.fn.destroy_command_pool)
        g_up.fn.destroy_command_pool(dev, g_up.pool, NULL);
    if (g_up.pipe_easu && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_easu, NULL);
    if (g_up.pipe_rcas && g_up.fn.destroy_pipeline)
        g_up.fn.destroy_pipeline(dev, g_up.pipe_rcas, NULL);
    if (g_up.playout && g_up.fn.destroy_pipeline_layout)
        g_up.fn.destroy_pipeline_layout(dev, g_up.playout, NULL);
    if (g_up.dsl && g_up.fn.destroy_descriptor_set_layout)
        g_up.fn.destroy_descriptor_set_layout(dev, g_up.dsl, NULL);
    memset(&g_up, 0, sizeof(g_up));
}

static int same_shape(const LslUpscaleShape *a, const LslUpscaleShape *b) {
    return a->format == b->format && a->real_w == b->real_w &&
           a->real_h == b->real_h && a->app_w == b->app_w && a->app_h == b->app_h;
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
    if (g_up.valid && g_up.dev == dev && same_shape(&g_up.shape, shape)) return 1;

    /* A different device means the old engine (pipelines, buffers, command
     * pools) cannot be reused here.  Drop it -- with its own device handle --
     * and build again.  A process with more than one live device gets one
     * engine at a time, which is the single-device limit noted in lsl_layer.c. */
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
    g_up.is16 = lsl_format_is_16f(shape->format);
    g_up.texel = lsl_format_texel_bytes(shape->format);
    g_up.plane_bytes = (VkDeviceSize)shape->real_w * shape->real_h * g_up.texel;
    g_up.stage_out_off = (VkDeviceSize)shape->app_w * shape->app_h * g_up.texel;
    g_up.stage_easu_off = g_up.stage_out_off + g_up.plane_bytes;
    g_up.next_slot = 0;

    VkPhysicalDevice pd = lsl_pd_of_dev(dev);
    if (pd == VK_NULL_HANDLE) pd = g_physdev;
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

    VkDescriptorSetLayoutBinding bind[2];
    memset(bind, 0, sizeof(bind));
    for (int k = 0; k < 2; ++k) {
        bind[k].binding = (uint32_t)k;
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

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_up.dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    UP_CHECK(g_up.fn.create_pipeline_layout(up_dev(), &plci, NULL, &g_up.playout),
             "create_pipeline_layout");

    if (g_up.is16) {
        g_up.pipe_easu = make_pipeline(lsl_spv_scale16_comp, lsl_spv_scale16_comp_words, g_up.playout);
        g_up.pipe_rcas = make_pipeline(lsl_spv_rcas16_comp, lsl_spv_rcas16_comp_words, g_up.playout);
    } else {
        g_up.pipe_easu = make_pipeline(lsl_spv_scale_comp, lsl_spv_scale_comp_words, g_up.playout);
        g_up.pipe_rcas = make_pipeline(lsl_spv_rcas_comp, lsl_spv_rcas_comp_words, g_up.playout);
    }
    if (!g_up.pipe_easu) UP_FAIL("EASU compute pipeline");
    if (!g_up.pipe_rcas) UP_FAIL("RCAS compute pipeline");

    for (int i = 0; i < UP_SLOTS; ++i) {
        UpSlot *s = &g_up.slot[i];
        /* `in` is copied into (TRANSFER_DST), copied out of for the dump and the
         * copy back (TRANSFER_SRC), and read and written by the shaders.  `mid`
         * is only ever touched by the shaders. */
        if (!make_buffer(g_up.plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s->in, &s->in_mem))
            UP_FAIL("device-local source buffer");
        if (!make_buffer(g_up.plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s->mid, &s->mid_mem))
            UP_FAIL("device-local scratch buffer");

        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        UP_CHECK(g_up.fn.create_fence(up_dev(), &fci, NULL, &s->fence), "create_fence");
    }

    /* Host-visible staging: the app's frame, then the upscaled frame, written
     * only on the frame that is dumped. */
    if (!make_buffer(g_up.stage_easu_off + g_up.plane_bytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &g_up.stage, &g_up.stage_mem))
        UP_FAIL("host-visible staging buffer");

    VkDescriptorPoolSize dps;
    memset(&dps, 0, sizeof(dps));
    dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    /* Two sets per slot -- one EASU, one RCAS -- each with two storage buffers. */
    dps.descriptorCount = 4 * UP_SLOTS;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 2 * UP_SLOTS;
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
    UP_CHECK(g_up.fn.allocate_descriptor_sets(up_dev(), &dsai, sets), "allocate EASU sets");
    for (int i = 0; i < UP_SLOTS; ++i) g_up.slot[i].set_easu = sets[i];
    UP_CHECK(g_up.fn.allocate_descriptor_sets(up_dev(), &dsai, sets), "allocate RCAS sets");
    for (int i = 0; i < UP_SLOTS; ++i) g_up.slot[i].set_rcas = sets[i];

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

    write_sets();
    g_up.valid = 1;
    lsl_log("scale: %ux%u -> %ux%u via FSR1 EASU+RCAS, %s pixels, queue family %u",
            shape->app_w, shape->app_h, shape->real_w, shape->real_h,
            g_up.is16 ? "half-float" : "RGBA8", g_up.family);
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
    pc.srcW = W; pc.srcH = H; pc.dstW = N; pc.dstH = M; pc.mode = 4;
    pc.sharpness = g_up.shape.sharpness;
    g_up.fn.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.pipe_easu);
    g_up.fn.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.playout,
                                  0, 1, &s->set_easu, 0, NULL);
    g_up.fn.cmd_push_constants(cmd, g_up.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    g_up.fn.cmd_dispatch(cmd, UP_GROUPS(N), UP_GROUPS(M), 1);

    /* The two dispatches share both buffers: EASU's output must be readable by
     * RCAS, and `in` -- which EASU read -- becomes RCAS's output.  One barrier
     * with two buffer entries expresses both dependencies. */
    {
        VkBufferMemoryBarrier bb[2];
        memset(bb, 0, sizeof(bb));
        bb[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb[0].buffer = s->mid;
        bb[0].offset = 0;
        bb[0].size = VK_WHOLE_SIZE;
        bb[1] = bb[0];
        bb[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bb[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb[1].buffer = s->in;
        g_up.fn.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                                  2, bb, 0, NULL);
    }

    pc.srcW = N; pc.srcH = M; pc.dstW = N; pc.dstH = M; pc.mode = 0;
    g_up.fn.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.pipe_rcas);
    g_up.fn.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.playout,
                                  0, 1, &s->set_rcas, 0, NULL);
    g_up.fn.cmd_push_constants(cmd, g_up.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    g_up.fn.cmd_dispatch(cmd, UP_GROUPS(N), UP_GROUPS(M), 1);

    up_buf_barrier(cmd, s->in, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    if (with_dump) {
        /* mid still holds the EASU result: RCAS only reads it.  Copying it out
         * as well lets tools/upscale-check.sh tell an EASU disagreement from an
         * RCAS one. */
        up_buf_barrier(cmd, s->mid, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
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
        g_up.fn.cmd_copy_buffer(cmd, s->in, g_up.stage, 1, &c);
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
    g_up.fn.cmd_copy_buffer_to_image(cmd, s->in, image,
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
