/*
 * lsl_layer.c -- Phase 2: frame injection for Steam/Proton games (dispatch).
 *
 * Phase 1 proved the scalers.  Phase 2 has to answer a different question: how
 * does a program that is *not* the game get GPU work in between the game's
 * render target and vkQueuePresentKHR, on Hyprland, without the game
 * cooperating?
 *
 * For a Vulkan game -- and for D3D games under Proton, because DXVK translates
 * to Vulkan and then goes through the loader -- the answer is a Vulkan layer.
 * This file is the plumbing: it loads into the instance/device chain, finds the
 * swapchain, and hands the interesting moments to the modules that do the work.
 *
 *   1. It observes every vkQueuePresentKHR (the interception point, proven
 *      before anything was built on it).
 *   2. With LSL_LAYER_CAPTURE=1 it rewrites the app's swapchain to add
 *      VK_IMAGE_USAGE_TRANSFER_SRC_BIT and copies the presented image back to a
 *      host buffer, logging a hash.  A hash is checkable from a log instead of
 *      by eye, which is the only way this can be tested with the monitor off.
 *   3. With LSL_LAYER_SCALE=<factor> it makes the app render at 1/factor of the
 *      surface resolution and upscales the frame back to full size with the
 *      Phase 1 FSR1 chain.  The engine lives in lsl_upscale.c.
 *
 * How the resize works, because it is the whole trick:
 *
 *   vkGetPhysicalDeviceSurfaceCapabilitiesKHR is the only place an app learns
 *   what size it should render at.  Reporting a smaller currentExtent (and a
 *   matching minImageExtent) makes the app build a smaller swapchain and render
 *   into it -- while the swapchain we actually create is still the surface's
 *   real size, because the lie is only in what we hand back to the app, not in
 *   what we pass down to the driver.
 *
 *   The app then renders its WxH image into the top-left WxH corner of the real
 *   NxM image.  At present time the layer upscales that corner over the whole
 *   image before the present goes through.  maxImageExtent is deliberately left
 *   alone, so an app that ignores currentExtent and asks for the full size
 *   simply gets no upscale instead of an error, and if creating the full-size
 *   swapchain is refused the layer quietly falls back to the app's size.
 *
 * Known limits, listed because they are the next steps rather than accidents:
 * no frame generation; global state, so one instance/device/at most four
 * swapchains -- and the upscale engine is bound to the most recently created
 * device, so a second live device presents unscaled rather than borrowing it;
 * the app is assumed to leave the presented image in
 * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, which is what every swapchain tutorial and
 * vkcube do; and a present from a different queue family than the one the
 * upscale runs on is declined rather than given an ownership transfer.
 *
 * Build: see CMakeLists.txt (target lsl-layer) -- it also writes the layer
 * manifest with an absolute library_path, which is the thing that stops most
 * hand-rolled layers from ever loading.
 *
 * Run:
 *   LSL_LAYER_LOG=/tmp/lsl.log LSL_LAYER_SCALE=2 \
 *   VK_ADD_LAYER_PATH=<build>/layer VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame \
 *   vkcube --c 30
 */

#include "lsl_layer.h"
#include "lsl_config.h"
#include "lsl_method.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------- state (see header) */

VkInstance g_instance;
VkPhysicalDevice g_physdev;
VkDevice g_device;
VkPhysicalDeviceMemoryProperties g_memprops;
int g_have_memprops;
PFN_vkGetInstanceProcAddr g_next_gipa;

static PFN_vkGetDeviceProcAddr g_next_gdpa;

/* ------------------------------------------------- instance/device registry
 *
 * A process can hold more than one VkInstance alive.  One appears on this host
 * -- another layer's private context -- and it makes a physical device of its
 * own but no swapchain.  Instance-level calls must be dispatched through the
 * instance that owns the physical device they name: resolving an entry point
 * through the wrong instance makes the loader walk the wrong chain, which
 * validation reports as VUID-...-commonparent and which -- that instance not
 * having enabled VK_KHR_surface -- the loader then refuses to execute at all,
 * handing back uninitialised capabilities.
 *
 * So the layer remembers which instance each physical device was enumerated
 * from, and which physical device each device was created from.  A one-instance
 * application never notices; the second one can no longer disturb the first.
 */

#define LSL_MAX_INSTS 4
#define LSL_MAX_PDS 8
#define LSL_MAX_DEVS 4

static struct {
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
} g_insts[LSL_MAX_INSTS];
static int g_inst_count;

static struct {
    VkPhysicalDevice pd;
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_caps;
} g_pds[LSL_MAX_PDS];
static int g_pd_count;

typedef struct NextDevice_s {
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
} NextDevice;

static struct {
    VkDevice dev;
    VkPhysicalDevice pd;
    PFN_vkGetDeviceProcAddr gdpa;
    NextDevice next;
} g_devs[LSL_MAX_DEVS];
static int g_dev_count;

/* The instance's own next-get-proc-addr, so lookups do not depend on whichever
 * instance happened to be created last. */
static PFN_vkGetInstanceProcAddr instance_gipa(VkInstance inst) {
    for (int i = 0; i < g_inst_count; ++i)
        if (g_insts[i].inst == inst) return g_insts[i].gipa;
    return g_next_gipa;
}

static void note_instance(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) {
    if (inst == VK_NULL_HANDLE) return;
    for (int i = 0; i < g_inst_count; ++i)
        if (g_insts[i].inst == inst) {
            g_insts[i].gipa = gipa;
            return;
        }
    if (g_inst_count >= LSL_MAX_INSTS) return;
    g_insts[g_inst_count].inst = inst;
    g_insts[g_inst_count].gipa = gipa;
    g_inst_count++;
}

static int pd_slot(VkPhysicalDevice pd) {
    for (int i = 0; i < g_pd_count; ++i)
        if (g_pds[i].pd == pd) return i;
    return -1;
}

static void note_physical_device(VkPhysicalDevice pd, VkInstance inst,
                                 PFN_vkGetInstanceProcAddr gipa) {
    if (pd == VK_NULL_HANDLE) return;
    int i = pd_slot(pd);
    if (i < 0) {
        if (g_pd_count >= LSL_MAX_PDS) return;
        i = g_pd_count++;
    }
    g_pds[i].pd = pd;
    g_pds[i].inst = inst;
    g_pds[i].gipa = gipa;
}

static VkInstance instance_of_pd(VkPhysicalDevice pd) {
    const int i = pd_slot(pd);
    return i >= 0 ? g_pds[i].inst : g_instance;
}

static PFN_vkGetInstanceProcAddr gipa_of_pd(VkPhysicalDevice pd) {
    const int i = pd_slot(pd);
    return i >= 0 ? g_pds[i].gipa : g_next_gipa;
}

PFN_vkVoidFunction lsl_pd_proc(VkPhysicalDevice pd, const char *name) {
    PFN_vkGetInstanceProcAddr gipa = gipa_of_pd(pd);
    if (!gipa) return NULL;
    return gipa(instance_of_pd(pd), name);
}

static void note_device(VkDevice dev, VkPhysicalDevice pd,
                        PFN_vkGetDeviceProcAddr gdpa) {
    if (dev == VK_NULL_HANDLE) return;
    for (int i = 0; i < g_dev_count; ++i)
        if (g_devs[i].dev == dev) {
            g_devs[i].pd = pd;
            g_devs[i].gdpa = gdpa;
            memset(&g_devs[i].next, 0, sizeof(NextDevice));
            return;
        }
    if (g_dev_count >= LSL_MAX_DEVS) return;
    g_devs[g_dev_count].dev = dev;
    g_devs[g_dev_count].pd = pd;
    g_devs[g_dev_count].gdpa = gdpa;
    memset(&g_devs[g_dev_count].next, 0, sizeof(NextDevice));
    g_dev_count++;
}

VkPhysicalDevice lsl_pd_of_dev(VkDevice dev) {
    for (int i = 0; i < g_dev_count; ++i)
        if (g_devs[i].dev == dev) return g_devs[i].pd;
    return VK_NULL_HANDLE;
}

/* Each device has its own next-chain GetDeviceProcAddr.  Entry points must be
 * resolved through the device they will be called with: the loader's
 * trampolines take the device as an argument, but a validation layer in the
 * chain keys its dispatch on the entry point's chain, and mixing the two aborts
 * (vvl::GetDispatchDevice). */
static PFN_vkGetDeviceProcAddr dev_gdpa(VkDevice dev) {
    for (int i = 0; i < g_dev_count; ++i)
        if (g_devs[i].dev == dev) return g_devs[i].gdpa;
    return g_next_gdpa;
}

/* Per-device cached next-chain entry points.  Using a global cache (as the
 * original code did) is unsafe when multiple devices exist: the second device
 * overwrites the cache and the first device ends up calling entry points from
 * the wrong chain, which can crash. */
static NextDevice *dev_next(VkDevice dev) {
    for (int i = 0; i < g_dev_count; ++i)
        if (g_devs[i].dev == dev) return &g_devs[i].next;
    return NULL;
}

static void forget_device(VkDevice dev) {
    for (int i = 0; i < g_dev_count; ++i)
        if (g_devs[i].dev == dev) {
            g_devs[i] = g_devs[--g_dev_count];
            return;
        }
}

static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_next_get_caps;

/* Resolve vkGetPhysicalDeviceSurfaceCapabilitiesKHR through the instance that
 * owns `pd`, once per physical device.  An unknown device falls back to the
 * current chain, which is what a single-instance process always gets. */
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_entry(VkPhysicalDevice pd) {
    const int i = pd_slot(pd);
    if (i >= 0) {
        if (!g_pds[i].get_caps && g_pds[i].gipa)
            g_pds[i].get_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
                g_pds[i].gipa(g_pds[i].inst,
                              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        return g_pds[i].get_caps;
    }
    if (!g_next_get_caps && g_next_gipa)
        g_next_get_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
            g_next_gipa(g_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return g_next_get_caps;
}

/* ------------------------------------------------------------------ logging */

static FILE *g_log;

static void log_open(void) {
    if (g_log) return;
    const char *path = getenv("LSL_LAYER_LOG");
    if (path && *path) {
        g_log = fopen(path, "a");
        if (g_log) return;
    }
    g_log = stderr;
}

void lsl_log(const char *fmt, ...) {
    /* A no-op process stays quiet.  This matters once the layer is installed as
     * an implicit layer, where it is loaded into every Vulkan application on the
     * machine and almost all of them are not games lsl was asked to touch. */
    if (!lsl_log_enabled()) return;
    va_list ap;
    log_open();
    fprintf(g_log, "[lsl-layer %ld] ", (long)getpid());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- env knobs */

int lsl_env_flag(const char *name) {
    const char *e = getenv(name);
    return (e && *e && *e != '0') ? 1 : 0;
}

const char *lsl_env_str(const char *name) {
    const char *e = getenv(name);
    return (e && *e) ? e : NULL;
}

/* ------------------------------------------------------------------- memory */

int lsl_pick_memory_type(uint32_t bits, VkMemoryPropertyFlags want,
                         VkMemoryPropertyFlags avoid, uint32_t *out) {
    if (!g_have_memprops) return 0;
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = g_memprops.memoryTypes[i].propertyFlags;
        if ((f & want) == want && !(f & avoid)) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ counters */

static uint64_t g_present_count;
static uint64_t g_captured;
static uint64_t g_upscaled;

/* ------------------------------------------------------------- configuration */

static int capture_enabled(void) {
    static int v = -1;
    if (v < 0) v = lsl_env_flag("LSL_LAYER_CAPTURE");
    return v;
}

/* Leave the image in TRANSFER_SRC_OPTIMAL instead of putting it back for the
 * app to present.  This exists so that "the layer is clean under validation" can
 * be told apart from "validation is not looking at the layer at all" -- without
 * a deliberate error, a clean run proves nothing. */
static int break_layout(void) {
    static int v = -1;
    if (v < 0) v = lsl_env_flag("LSL_LAYER_BREAK_LAYOUT");
    return v;
}

/* Reading the image back blocks the queue on a fence every frame, which slows
 * the application down to a crawl -- visible as a stuttering window.  The
 * prototype therefore stops after a handful of frames. */
static uint64_t capture_cap(void) {
    static uint64_t v;
    static int read;
    if (!read) {
        const char *e = getenv("LSL_LAYER_CAPTURE_FRAMES");
        long n = e && *e ? strtol(e, NULL, 10) : 12;
        v = (n > 0) ? (uint64_t)n : 0;
        read = 1;
    }
    return v;
}

static int capture_wanted(void) {
    uint64_t cap = capture_cap();
    if (cap && g_captured >= cap) {
        if (g_captured == cap) {
            lsl_log("readback stopped after %llu frame(s) "
                    "(LSL_LAYER_CAPTURE_FRAMES=0 for no limit)",
                    (unsigned long long)cap);
            g_captured++; /* log it once */
        }
        return 0;
    }
    return 1;
}

/* The settings for this process, resolved once, in lsl_config.c.  Both ways in
 * -- an explicit LSL_LAYER_SCALE, or the GUI's profile for this SteamAppId --
 * end up here, and everything below treats them the same. */
static float upscale_scale(void) { return lsl_config()->scale; }

static int scaling(void) { return lsl_config()->enabled; }

static float upscale_sharpness(void) { return lsl_config()->sharpness; }

static uint32_t upscale_mode(void) { return lsl_config()->mode; }

/* ------------------------------------------------------------------- chains */

#define LSL_MAX_SWAPCHAINS 4
/* Drivers return 2-5 images for a normal application, and more for mailbox or
 * an application that asks for a deep queue.  The count is only ever a cache of
 * what vkGetSwapchainImagesKHR returned; when a swapchain reports more than
 * this, the extra indices are presented un-upscaled and the log says so. */
#define LSL_MAX_IMAGES 16

typedef struct {
    VkSwapchainKHR sc;
    VkDevice dev;                    /* the device that created it */
    VkFormat format;
    uint32_t width, height;          /* the real swapchain extent (N x M) */
    uint32_t app_width, app_height;  /* what the application believes (W x H) */
    int upscale;
    uint32_t image_count;
    VkImage images[LSL_MAX_IMAGES];
    int have_images;
    /* Counts presents, so that when every slot is taken the stalest swapchain is
     * the one recycled.  A window being dragged recreates its swapchain faster
     * than a slot can be freed if the application never destroys the old ones,
     * and recycling the one that is actually being presented would silently stop
     * upscaling it. */
    uint64_t used;
} Chain;

static Chain g_chains[LSL_MAX_SWAPCHAINS];
static uint64_t g_chain_clock;

static Chain *chain_for(VkSwapchainKHR sc) {
    if (sc == VK_NULL_HANDLE) return NULL;
    for (int i = 0; i < LSL_MAX_SWAPCHAINS; ++i)
        if (g_chains[i].sc == sc) return &g_chains[i];
    return NULL;
}

static void chain_forget(VkSwapchainKHR sc) {
    Chain *c = chain_for(sc);
    if (c) memset(c, 0, sizeof(*c));
}

static Chain *chain_alloc(void) {
    for (int i = 0; i < LSL_MAX_SWAPCHAINS; ++i)
        if (g_chains[i].sc == VK_NULL_HANDLE) return &g_chains[i];
    /* Every slot is taken.  Recycle the least recently presented one -- with a
     * normal application that is a swapchain that was left behind, not the one
     * on screen. */
    int oldest = 0;
    for (int i = 1; i < LSL_MAX_SWAPCHAINS; ++i)
        if (g_chains[i].used < g_chains[oldest].used) oldest = i;
    Chain *c = &g_chains[oldest];
    if (c->sc != VK_NULL_HANDLE)
        lsl_log("scale: recycling the slot for swapchain %p (%ux%u): more than "
                "%d live swapchains", (void *)c->sc, c->width, c->height,
                LSL_MAX_SWAPCHAINS);
    memset(c, 0, sizeof(*c));
    return c;
}

/* -------------------------------------------------------------- queue family */

/* The upscale has to run on the application's present queue, and the image
 * barriers assume that queue belongs to the family the engine picked.  So the
 * queue handles the app asks for are remembered together with their family. */
#define LSL_MAX_QUEUES 8

typedef struct {
    VkQueue queue;
    uint32_t family;
} QueueEntry;

static QueueEntry g_queues[LSL_MAX_QUEUES];
static int g_queue_count;

static void note_queue(VkQueue q, uint32_t family) {
    if (q == VK_NULL_HANDLE) return;
    for (int i = 0; i < g_queue_count; ++i)
        if (g_queues[i].queue == q) {
            g_queues[i].family = family;
            return;
        }
    if (g_queue_count < LSL_MAX_QUEUES)
        g_queues[g_queue_count++] = (QueueEntry){q, family};
}

static uint32_t family_of_queue(VkQueue q) {
    for (int i = 0; i < g_queue_count; ++i)
        if (g_queues[i].queue == q) return g_queues[i].family;
    return UINT32_MAX;
}

/* ---------------------------------------------------------- device entry pts */


/* Every next-chain entry point the injected passes need, resolved once at device
 * creation.  Resolving once keeps dozens of GetDeviceProcAddr calls out of
 * vkQueuePresentKHR, which is the hot path.  See the header for why the
 * loader's exported entry points are not an option.
 *
 * `out` is filled from `dev`'s own next-get-proc-addr: a process can hold more
 * than one live device, and each has its own chain, so a table resolved for one
 * device must not be used to call another (VVL aborts on the mismatch).  The
 * upscale engine keeps its own table for this reason. */
void lsl_resolve_device_fns(VkDevice dev, LslDevFns *out) {
    PFN_vkGetDeviceProcAddr gdpa = dev_gdpa(dev);
    if (!gdpa) gdpa = g_next_gdpa;
    memset(out, 0, sizeof(*out));
#define LSL_FN(name, field) out->field = (PFN_##name)gdpa(dev, #name)
    LSL_FN(vkCreateCommandPool, create_command_pool);
    LSL_FN(vkDestroyCommandPool, destroy_command_pool);
    LSL_FN(vkAllocateCommandBuffers, allocate_command_buffers);
    LSL_FN(vkResetCommandBuffer, reset_command_buffer);
    LSL_FN(vkBeginCommandBuffer, begin_command_buffer);
    LSL_FN(vkEndCommandBuffer, end_command_buffer);
    LSL_FN(vkCmdPipelineBarrier, cmd_pipeline_barrier);
    LSL_FN(vkCmdCopyImageToBuffer, cmd_copy_image_to_buffer);
    LSL_FN(vkCmdCopyBufferToImage, cmd_copy_buffer_to_image);
    LSL_FN(vkCmdCopyBuffer, cmd_copy_buffer);
    LSL_FN(vkCmdBindPipeline, cmd_bind_pipeline);
    LSL_FN(vkCmdBindDescriptorSets, cmd_bind_descriptor_sets);
    LSL_FN(vkCmdPushConstants, cmd_push_constants);
    LSL_FN(vkCmdDispatch, cmd_dispatch);
    LSL_FN(vkCreateFence, create_fence);
    LSL_FN(vkDestroyFence, destroy_fence);
    LSL_FN(vkResetFences, reset_fences);
    LSL_FN(vkWaitForFences, wait_for_fences);
    LSL_FN(vkQueueSubmit, queue_submit);
    LSL_FN(vkMapMemory, map_memory);
    LSL_FN(vkUnmapMemory, unmap_memory);
    LSL_FN(vkCreateBuffer, create_buffer);
    LSL_FN(vkDestroyBuffer, destroy_buffer);
    LSL_FN(vkGetBufferMemoryRequirements, get_buffer_memory_requirements);
    LSL_FN(vkAllocateMemory, allocate_memory);
    LSL_FN(vkFreeMemory, free_memory);
    LSL_FN(vkBindBufferMemory, bind_buffer_memory);
    LSL_FN(vkCreateShaderModule, create_shader_module);
    LSL_FN(vkDestroyShaderModule, destroy_shader_module);
    LSL_FN(vkCreateDescriptorSetLayout, create_descriptor_set_layout);
    LSL_FN(vkDestroyDescriptorSetLayout, destroy_descriptor_set_layout);
    LSL_FN(vkCreatePipelineLayout, create_pipeline_layout);
    LSL_FN(vkDestroyPipelineLayout, destroy_pipeline_layout);
    LSL_FN(vkCreateComputePipelines, create_compute_pipelines);
    LSL_FN(vkDestroyPipeline, destroy_pipeline);
    LSL_FN(vkCreateDescriptorPool, create_descriptor_pool);
    LSL_FN(vkDestroyDescriptorPool, destroy_descriptor_pool);
    LSL_FN(vkAllocateDescriptorSets, allocate_descriptor_sets);
    LSL_FN(vkUpdateDescriptorSets, update_descriptor_sets);
#undef LSL_FN
}

/* Every command buffer the loader hands to the application has the device's
 * dispatch table stamped into its first word (loader_set_dispatch).  The
 * validation layer finds a command buffer's device through that word
 * (vvl::GetDispatchDevice -> GetDispatchKey), and so does the loader itself.
 *
 * A layer that allocates its own command buffers through the next-chain entry
 * point bypasses the loader's vkAllocateCommandBuffers trampoline, so nothing
 * stamps them: the word is whatever the driver left there.  The driver does not
 * care (the word is reserved for exactly this) but the validation layer aborts
 * with "The VkDevice dispatch handle was not found" on the first command that
 * takes one of those buffers.  Stamp them with the same value the loader would
 * have used -- the device's own first word -- and the handle spaces agree again.
 * See loader/loader.h (loader_set_dispatch) and loader/README.md, ICD_LOADER_MAGIC. */
void lsl_stamp_command_buffers(VkDevice dev, uint32_t count,
                               const VkCommandBuffer *cbs) {
    void *disp = dev ? *(void **)(void *)dev : NULL;
    if (!disp) return;
    for (uint32_t i = 0; i < count; ++i)
        if (cbs[i]) *(void **)(void *)cbs[i] = disp;
}

/* ----------------------------------------------------------------- capture */

typedef struct {
    VkDevice dev;           /* the device every object below belongs to */
    LslDevFns fn;           /* and its entry points */
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
} Capture;

static Capture g_cap;

/* Destroy the capture objects on the device they belong to.  This runs from
 * vkDestroyDevice, while that device is still alive, and does nothing when the
 * device going away is not the one holding them. */
static void capture_end(VkDevice dev) {
    if (g_cap.dev != dev) return;
    if (g_cap.buf && g_cap.fn.destroy_buffer) g_cap.fn.destroy_buffer(dev, g_cap.buf, NULL);
    if (g_cap.mem && g_cap.fn.free_memory) g_cap.fn.free_memory(dev, g_cap.mem, NULL);
    if (g_cap.fence && g_cap.fn.destroy_fence) g_cap.fn.destroy_fence(dev, g_cap.fence, NULL);
    if (g_cap.pool && g_cap.fn.destroy_command_pool) g_cap.fn.destroy_command_pool(dev, g_cap.pool, NULL);
    memset(&g_cap, 0, sizeof(g_cap));
}

/* A host-visible buffer large enough to hold the whole image.  Grown, never
 * shrunk, so a resize does not churn allocations.
 *
 * Everything here belongs to `dev`, the swapchain's own device.  A second live
 * device is normal (another implicit layer creates one), and its objects are not
 * interchangeable with the first device's: mixing them is invalid and
 * validation says so (VUID-vkCmdCopyImageToBuffer-commonparent).  So the entry
 * point table is resolved per device and the objects are rebuilt when the device
 * changes -- the swapchain's, never whichever device was created last. */
static int ensure_capture(VkDevice dev, VkDeviceSize size) {
    if (dev == VK_NULL_HANDLE) return 0;
    if (g_cap.dev != dev) {
        capture_end(g_cap.dev);
        lsl_resolve_device_fns(dev, &g_cap.fn);
        g_cap.dev = dev;
    }

    if (g_cap.fn.create_buffer && g_cap.buf && g_cap.size < size) {
        g_cap.fn.destroy_buffer(dev, g_cap.buf, NULL);
        g_cap.fn.free_memory(dev, g_cap.mem, NULL);
        g_cap.buf = VK_NULL_HANDLE;
        g_cap.mem = VK_NULL_HANDLE;
        g_cap.size = 0;
    }
    if (g_cap.buf) return 1;
    if (!g_cap.fn.create_buffer || !g_cap.fn.get_buffer_memory_requirements ||
        !g_cap.fn.allocate_memory || !g_cap.fn.bind_buffer_memory ||
        !g_cap.fn.create_command_pool || !g_cap.fn.allocate_command_buffers ||
        !g_cap.fn.create_fence)
        return 0;

    if (!g_cap.pool) {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (g_cap.fn.create_command_pool(dev, &pci, NULL, &g_cap.pool) != VK_SUCCESS) return 0;

        VkCommandBufferAllocateInfo cbi;
        memset(&cbi, 0, sizeof(cbi));
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = g_cap.pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        if (g_cap.fn.allocate_command_buffers(dev, &cbi, &g_cap.cmd) != VK_SUCCESS) return 0;
        /* Validation resolves a command buffer's device through its first word;
         * the loader stamps it for the application, but not for us. */
        lsl_stamp_command_buffers(dev, 1, &g_cap.cmd);

        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (g_cap.fn.create_fence(dev, &fci, NULL, &g_cap.fence) != VK_SUCCESS) return 0;
    }

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_cap.fn.create_buffer(dev, &bci, NULL, &g_cap.buf) != VK_SUCCESS) return 0;

    VkMemoryRequirements mr;
    g_cap.fn.get_buffer_memory_requirements(dev, g_cap.buf, &mr);
    uint32_t type;
    if (!lsl_pick_memory_type(mr.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              0, &type))
        return 0;

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    if (g_cap.fn.allocate_memory(dev, &mai, NULL, &g_cap.mem) != VK_SUCCESS) return 0;
    if (g_cap.fn.bind_buffer_memory(dev, g_cap.buf, g_cap.mem, 0) != VK_SUCCESS) return 0;
    g_cap.size = size;
    return 1;
}

#define HAVE(field) (g_cap.fn.field != NULL)

static int capture_frame(VkQueue queue, Chain *c, uint32_t index) {
    if (index >= c->image_count || !c->have_images) return 0;

    const uint32_t texel = lsl_format_texel_bytes(c->format);
    VkDeviceSize size = (VkDeviceSize)c->width * c->height * texel;
    if (size == 0) return 0;
    if (!ensure_capture(c->dev, size)) return 0;

    if (!HAVE(reset_command_buffer) || !HAVE(begin_command_buffer) ||
        !HAVE(cmd_pipeline_barrier) || !HAVE(cmd_copy_image_to_buffer) ||
        !HAVE(end_command_buffer) || !HAVE(reset_fences) || !HAVE(queue_submit) ||
        !HAVE(wait_for_fences) || !HAVE(map_memory) || !HAVE(unmap_memory))
        return 0;

    g_cap.fn.reset_command_buffer(g_cap.cmd, 0);

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_cap.fn.begin_command_buffer(g_cap.cmd, &bi);

    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = c->images[index];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    g_cap.fn.cmd_pipeline_barrier(g_cap.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = c->width;
    region.imageExtent.height = c->height;
    region.imageExtent.depth = 1;
    g_cap.fn.cmd_copy_image_to_buffer(g_cap.cmd, c->images[index],
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cap.buf, 1, &region);

    if (break_layout()) {
        lsl_log("LSL_LAYER_BREAK_LAYOUT: leaving the image in TRANSFER_SRC_OPTIMAL "
                "(deliberate; validation should object)");
    } else {
        VkImageMemoryBarrier back = b;
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask = 0;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        g_cap.fn.cmd_pipeline_barrier(g_cap.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      0, 0, NULL, 0, NULL, 1, &back);
    }

    g_cap.fn.end_command_buffer(g_cap.cmd);

    g_cap.fn.reset_fences(c->dev, 1, &g_cap.fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cap.cmd;
    if (g_cap.fn.queue_submit(queue, 1, &si, g_cap.fence) != VK_SUCCESS) return 0;

    if (g_cap.fn.wait_for_fences(c->dev, 1, &g_cap.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        lsl_log("capture: timed out waiting for the copy to finish");
        return 0;
    }

    /* FNV-1a over the whole image.  Hashing only a prefix is a trap: the first
     * rows of a 3D scene are usually flat background, so the hash stops moving
     * even though the frame is animating. */
    void *p = NULL;
    if (g_cap.fn.map_memory(c->dev, g_cap.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    const uint8_t *bytes = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (VkDeviceSize i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    g_cap.fn.unmap_memory(c->dev, g_cap.mem);
    lsl_log("captured frame %llu: %ux%u, hash %016llx",
            (unsigned long long)g_present_count, c->width, c->height,
            (unsigned long long)h);
    return 1;
}

/* ----------------------------------------------------- next instance pointers */

/* --------------------------------------------------------------- the resize */

/* What the layer knows about each surface, per surface.
 *
 * The resize needs two numbers: how big the surface really is, and how big the
 * application was told it is (the lie).  The first one is not always available:
 *
 *   - an X11 window reports its size, but only once the window manager has
 *     mapped it -- early capabilities calls say 0x0;
 *   - a Wayland surface never reports a size at all: currentExtent is
 *     UINT32_MAX and the application picks whatever it likes, so the first
 *     swapchain's own extent is the only place the size can be learned from.
 *
 * So the lie is armed from whichever source has the answer, and the swapchain
 * the application asks for afterwards is the confirmation that it took the
 * lie.  When it did, the real size is the remembered one -- not the lie times
 * the scale, which would drift by a pixel or two because the division floors. */
#define LSL_MAX_SURFACES 4
static struct {
    VkSurfaceKHR surface;
    uint32_t real_w, real_h;   /* the surface's true size, once known */
    uint32_t lied_w, lied_h;   /* what the application was last told */
} g_lied[LSL_MAX_SURFACES];

static int lie_slot(VkSurfaceKHR surface) {
    if (surface == VK_NULL_HANDLE) return -1;
    for (int i = 0; i < LSL_MAX_SURFACES; ++i)
        if (g_lied[i].surface == surface) return i;
    for (int i = 0; i < LSL_MAX_SURFACES; ++i)
        if (g_lied[i].surface == VK_NULL_HANDLE) {
            g_lied[i].surface = surface;
            return i;
        }
    return -1;
}

static void lie_set_real(VkSurfaceKHR surface, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    const int i = lie_slot(surface);
    if (i < 0) return;
    if (g_lied[i].real_w != w || g_lied[i].real_h != h) {
        g_lied[i].real_w = w;
        g_lied[i].real_h = h;
        g_lied[i].lied_w = g_lied[i].lied_h = 0;   /* the old lie is stale */
    }
}

/* The real size, if a previous call worked it out. */
static int lie_real(VkSurfaceKHR surface, uint32_t *w, uint32_t *h) {
    for (int i = 0; i < LSL_MAX_SURFACES; ++i)
        if (g_lied[i].surface == surface && g_lied[i].real_w && g_lied[i].real_h) {
            *w = g_lied[i].real_w;
            *h = g_lied[i].real_h;
            return 1;
        }
    return 0;
}

/* Did the application ask for exactly what we last told it?  Then it took the
 * lie, and the surface really is the remembered size. */
static int lie_taken(VkSurfaceKHR surface, uint32_t w, uint32_t h) {
    for (int i = 0; i < LSL_MAX_SURFACES; ++i)
        if (g_lied[i].surface == surface && g_lied[i].lied_w == w && g_lied[i].lied_h == h &&
            g_lied[i].lied_w != 0)
            return 1;
    return 0;
}

/* Shrink what the application is told the surface is.  minImageExtent has to
 * move with currentExtent or the answer is self-inconsistent (the spec requires
 * minImageExtent <= currentExtent <= maxImageExtent); maxImageExtent is left
 * alone on purpose, see the file comment. */
static void shrink_caps(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
    const float s = upscale_scale();
    if (!(s > 1.0f)) return;

    uint32_t real_w = 0, real_h = 0;
    if (caps->currentExtent.width != UINT32_MAX && caps->currentExtent.height != UINT32_MAX &&
        caps->currentExtent.width > 0 && caps->currentExtent.height > 0) {
        /* The driver is telling us the surface size outright. */
        real_w = caps->currentExtent.width;
        real_h = caps->currentExtent.height;
    } else if (!lie_real(surface, &real_w, &real_h)) {
        /* Nothing has named a size yet: a Wayland surface will not, so wait for
         * the application to create a swapchain and learn it from there. */
        static int warned;
        if (!warned) {
            warned = 1;
            lsl_log("scale: the surface does not name a size (min %ux%u, max %ux%u, "
                    "current %ux%u); the first swapchain will settle it, so the first "
                    "frame renders at full size",
                    caps->minImageExtent.width, caps->minImageExtent.height,
                    caps->maxImageExtent.width, caps->maxImageExtent.height,
                    caps->currentExtent.width, caps->currentExtent.height);
        }
        return;
    }
    lie_set_real(surface, real_w, real_h);

    uint32_t w = (uint32_t)((float)real_w / s);
    uint32_t h = (uint32_t)((float)real_h / s);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w >= real_w && h >= real_h) return;

    static int logged;
    if (!logged) {
        logged = 1;
        lsl_log("resize: telling the application %ux%u instead of %ux%u (scale %.2f)",
                w, h, real_w, real_h, (double)s);
    }

    const int i = lie_slot(surface);
    if (i >= 0) {
        g_lied[i].lied_w = w;
        g_lied[i].lied_h = h;
    }
    caps->currentExtent.width = w;
    caps->currentExtent.height = h;
    caps->minImageExtent.width = w;
    caps->minImageExtent.height = h;
}

VKAPI_ATTR VkResult VKAPI_CALL lsl_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next = caps_entry(pd);
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(pd, surface, caps);
    if (r == VK_SUCCESS && caps) shrink_caps(surface, caps);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL lsl_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info,
    VkSurfaceCapabilities2KHR *caps) {
    PFN_vkGetInstanceProcAddr gipa = gipa_of_pd(pd);
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR next =
        gipa ? (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
                   gipa(instance_of_pd(pd), "vkGetPhysicalDeviceSurfaceCapabilities2KHR")
             : NULL;
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(pd, info, caps);
    if (r == VK_SUCCESS && caps) shrink_caps(info ? info->surface : VK_NULL_HANDLE,
                                             &caps->surfaceCapabilities);
    return r;
}

/* --------------------------------------------------------------- wrappers */

VKAPI_ATTR VkResult VKAPI_CALL lsl_CreateInstance(const VkInstanceCreateInfo *ci,
                                                  const VkAllocationCallbacks *ac,
                                                  VkInstance *out) {
    VkLayerInstanceCreateInfo *link = NULL;
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            VkLayerInstanceCreateInfo *i = (VkLayerInstanceCreateInfo *)s;
            if (i->function == VK_LAYER_LINK_INFO) {
                link = i;
                break;
            }
        }
    }
    if (!link || !link->u.pLayerInfo) {
        lsl_log("vkCreateInstance: no layer link info in the pNext chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    /* Consume our link so the next layer/ICD in the chain sees the right one. */
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next = (PFN_vkCreateInstance)g_next_gipa(NULL, "vkCreateInstance");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = next(ci, ac, out);
    if (r == VK_SUCCESS) {
        note_instance(*out, g_next_gipa);
        if (g_instance == VK_NULL_HANDLE) g_instance = *out;
    }
    lsl_log("vkCreateInstance -> %d", (int)r);
    return r;
}

/* Remember which instance each physical device came from, so later instance
 * calls can be dispatched through the right chain.  A layer only sees this at
 * all because it hands its own entry point back from vkGetInstanceProcAddr. */
VKAPI_ATTR VkResult VKAPI_CALL lsl_EnumeratePhysicalDevices(VkInstance inst,
                                                            uint32_t *count,
                                                            VkPhysicalDevice *pds) {
    PFN_vkGetInstanceProcAddr gipa = instance_gipa(inst);
    PFN_vkEnumeratePhysicalDevices next =
        gipa ? (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices")
             : NULL;
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(inst, count, pds);
    if (r == VK_SUCCESS && count && pds)
        for (uint32_t i = 0; i < *count; ++i) note_physical_device(pds[i], inst, gipa);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL lsl_CreateDevice(VkPhysicalDevice pd,
                                                const VkDeviceCreateInfo *ci,
                                                const VkAllocationCallbacks *ac,
                                                VkDevice *out) {
    VkLayerDeviceCreateInfo *link = NULL;
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            VkLayerDeviceCreateInfo *i = (VkLayerDeviceCreateInfo *)s;
            if (i->function == VK_LAYER_LINK_INFO) {
                link = i;
                break;
            }
        }
    }
    if (!link || !link->u.pLayerInfo) {
        lsl_log("vkCreateDevice: no layer link info in the pNext chain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    /* Resolve through the instance that owns this physical device, not through
     * whichever instance was created last. */
    PFN_vkGetInstanceProcAddr gipa = gipa_of_pd(pd);
    VkInstance inst = instance_of_pd(pd);

    PFN_vkCreateDevice next =
        gipa ? (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice") : NULL;
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = next(pd, ci, ac, out);
    if (r == VK_SUCCESS) {
        g_device = *out;
        g_physdev = pd;
        note_device(*out, pd, g_next_gdpa);

        PFN_vkGetPhysicalDeviceMemoryProperties get_memprops =
            gipa ? (PFN_vkGetPhysicalDeviceMemoryProperties)
                       gipa(inst, "vkGetPhysicalDeviceMemoryProperties")
                 : NULL;
        if (get_memprops) {
            get_memprops(pd, &g_memprops);
            g_have_memprops = 1;
        }
    }
    lsl_log("vkCreateDevice -> %d (device %p)", (int)r, (void *)*out);
    return r;
}

VKAPI_ATTR void VKAPI_CALL lsl_DestroyDevice(VkDevice dev,
                                             const VkAllocationCallbacks *ac) {
    lsl_log("vkDestroyDevice(%p)", (void *)dev);
    /* Only the engine's own device tears the engine down: another live device
     * in the same process must not free it. */
    if (lsl_upscale_device() == dev) lsl_upscale_end();
    capture_end(dev);
    /* Resolve the device's own chain *before* forget_device() removes it from
     * g_devs[]: afterwards dev_gdpa() falls back to the global (last-created)
     * chain, which can belong to a frame-generation layer's helper device and
     * make the forward below skip that layer's teardown -- leaking its objects. */
    PFN_vkGetDeviceProcAddr gdpa = dev_gdpa(dev);
    /* Original behavior: clear all chains.  Per-device filtering was
     * attempted but QueueEntry has no dev field; the memset is restored
     * because the crash investigation showed the issue is the static function
     * pointers, not the chain clearing. */
    memset(g_chains, 0, sizeof(g_chains));
    memset(g_queues, 0, sizeof(g_queues));
    g_queue_count = 0;
    forget_device(dev);
    if (!gdpa) return;
    PFN_vkDestroyDevice next = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    if (next) next(dev, ac);
}

VKAPI_ATTR void VKAPI_CALL lsl_GetDeviceQueue(VkDevice dev, uint32_t family,
                                              uint32_t index, VkQueue *queue) {
    static PFN_vkGetDeviceQueue next;
    if (!next) next = (PFN_vkGetDeviceQueue)g_next_gdpa(dev, "vkGetDeviceQueue");
    if (!next) return;
    next(dev, family, index, queue);
    if (queue) note_queue(*queue, family);
}

VKAPI_ATTR void VKAPI_CALL lsl_GetDeviceQueue2(VkDevice dev,
                                               const VkDeviceQueueInfo2 *info,
                                               VkQueue *queue) {
    PFN_vkGetDeviceQueue2 next = (PFN_vkGetDeviceQueue2)dev_gdpa(dev)(dev, "vkGetDeviceQueue2");
    if (!next) return;
    next(dev, info, queue);
    if (queue && info) note_queue(*queue, info->queueFamilyIndex);
}

/* Rewrites the application's swapchain: a larger real extent when upscaling is
 * on, plus the usage bits the injected passes need.  Both are real changes to
 * the app's swapchain, and the reason the layer is opt-in. */
VKAPI_ATTR VkResult VKAPI_CALL lsl_CreateSwapchainKHR(VkDevice dev,
                                                      const VkSwapchainCreateInfoKHR *ci,
                                                      const VkAllocationCallbacks *ac,
                                                      VkSwapchainKHR *out) {
    NextDevice *nd = dev_next(dev);
    if (!nd) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!nd->create_swapchain)
        nd->create_swapchain = (PFN_vkCreateSwapchainKHR)dev_gdpa(dev)(dev, "vkCreateSwapchainKHR");
    if (!nd->create_swapchain) return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkSwapchainCreateInfoKHR local = *ci;
    const int want_copy = capture_enabled() || scaling();
    if (want_copy) {
        local.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        local.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    uint32_t real_w = ci->imageExtent.width;
    uint32_t real_h = ci->imageExtent.height;
    int want_upscale = 0;

    /* The capability callback is normally the first surface call an application
     * makes, but it does not have to be, and we need the driver's answer to know
     * how big the surface really is.  Resolve it through the swapchain's own
     * device here too, so the resize does not silently depend on call order. */
    VkPhysicalDevice pd = lsl_pd_of_dev(dev);
    if (pd == VK_NULL_HANDLE) pd = lsl_pd_of_dev(dev);
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_caps = caps_entry(pd);

    if (scaling() && lsl_format_supported(ci->imageFormat) && get_caps &&
        pd != VK_NULL_HANDLE && ci->surface != VK_NULL_HANDLE) {
        VkSurfaceCapabilitiesKHR caps;
        memset(&caps, 0, sizeof(caps));
        VkResult cr = get_caps(pd, ci->surface, &caps);

        /* Prefer the driver's answer -- it is exact.  It is only usable while
         * the driver names a real size; otherwise the surface's size has to come
         * from the remembered lie, or be learned from the application right
         * here. */
        uint32_t nw = 0, nh = 0;
        if (cr == VK_SUCCESS && caps.currentExtent.width != UINT32_MAX &&
            caps.currentExtent.height != UINT32_MAX && caps.currentExtent.width > 0 &&
            caps.currentExtent.height > 0) {
            nw = caps.currentExtent.width;
            nh = caps.currentExtent.height;
            lie_set_real(ci->surface, nw, nh);
        } else if (lie_taken(ci->surface, ci->imageExtent.width, ci->imageExtent.height)) {
            lie_real(ci->surface, &nw, &nh);
        } else if (!lie_real(ci->surface, &nw, &nh)) {
            /* Nothing has told us the surface size and the application has not
             * been lied to (it is asking for the full size).  This is a Wayland
             * surface on its first swapchain: the application's own extent is
             * the surface size, so remember it and let the resize start the next
             * time the application asks what the surface is. */
            lie_set_real(ci->surface, ci->imageExtent.width, ci->imageExtent.height);
            nw = nh = 0;
            static int taught;
            if (!taught) {
                taught = 1;
                lsl_log("scale: learned the surface size %ux%u from the first "
                        "swapchain; upscaling starts on the next one",
                        ci->imageExtent.width, ci->imageExtent.height);
            }
        }

        if (nw > ci->imageExtent.width && nh > ci->imageExtent.height) {
            LslUpscaleShape shape;
            memset(&shape, 0, sizeof(shape));
            shape.format = ci->imageFormat;
            shape.real_w = nw;
            shape.real_h = nh;
            shape.app_w = ci->imageExtent.width;
            shape.app_h = ci->imageExtent.height;
            shape.sharpness = upscale_sharpness();
            shape.mode = upscale_mode();
            if (lsl_upscale_begin(&shape, ci->surface, dev)) {
                local.imageExtent.width = nw;
                local.imageExtent.height = nh;
                real_w = nw;
                real_h = nh;
                want_upscale = 1;
            }
        } else {
            static int warned;
            if (!warned) {
                warned = 1;
                if (nw == 0)
                    lsl_log("scale: the surface never named a size the layer could "
                            "use (capabilities said %ux%u); nothing to upscale",
                            caps.currentExtent.width, caps.currentExtent.height);
                else
                    lsl_log("scale: the application asked for %ux%u on a %ux%u "
                            "surface; nothing to upscale",
                            ci->imageExtent.width, ci->imageExtent.height, nw, nh);
            }
        }
    } else if (scaling()) {
        static int warned;
        if (!warned) {
            warned = 1;
            lsl_log("scale: cannot resize this swapchain (format %d, caps %p, "
                    "physical device %p, surface %p)",
                    (int)ci->imageFormat, (void *)get_caps,
                    (void *)pd, (void *)ci->surface);
        }
    }

    VkResult r = nd->create_swapchain(dev, &local, ac, out);
    if (r != VK_SUCCESS && want_upscale) {
        /* The surface can have resized between the capability query and now, or
         * the driver can simply refuse the larger extent.  Losing the upscale is
         * better than losing the frame. */
        lsl_log("scale: creating a %ux%u swapchain failed (%d); retrying at the "
                "application's %ux%u", real_w, real_h, (int)r,
                ci->imageExtent.width, ci->imageExtent.height);
        local = *ci;
        if (want_copy) {
            local.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            local.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        want_upscale = 0;
        real_w = ci->imageExtent.width;
        real_h = ci->imageExtent.height;
        r = nd->create_swapchain(dev, &local, ac, out);
    }

    if (r == VK_SUCCESS) {
        chain_forget(ci->oldSwapchain);
        Chain *c = chain_alloc();
        c->sc = *out;
        c->dev = dev;
        c->format = ci->imageFormat;
        c->width = real_w;
        c->height = real_h;
        c->app_width = ci->imageExtent.width;
        c->app_height = ci->imageExtent.height;
        c->upscale = want_upscale;
        c->used = ++g_chain_clock;
        lsl_log("vkCreateSwapchainKHR: %ux%u (application believes %ux%u, fmt %d) "
                "on device %p%s",
                real_w, real_h, ci->imageExtent.width, ci->imageExtent.height,
                (int)ci->imageFormat, (void *)dev, want_upscale ? " - upscaling" : "");
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL lsl_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc,
                                                   const VkAllocationCallbacks *ac) {
    Chain *c = chain_for(sc);
    /* The application waited for its own submission, not for the dispatch this
     * layer submitted after it at present time, which reads these images. */
    if (c && c->upscale && c->dev == lsl_upscale_device()) lsl_upscale_drain();
    chain_forget(sc);
    NextDevice *nd = dev_next(dev);
    if (nd && !nd->destroy_swapchain)
        nd->destroy_swapchain = (PFN_vkDestroySwapchainKHR)dev_gdpa(dev)(dev, "vkDestroySwapchainKHR");
    if (nd && nd->destroy_swapchain) nd->destroy_swapchain(dev, sc, ac);
}

VKAPI_ATTR VkResult VKAPI_CALL lsl_GetSwapchainImagesKHR(VkDevice dev,
                                                         VkSwapchainKHR sc,
                                                         uint32_t *count,
                                                         VkImage *images) {
    NextDevice *nd = dev_next(dev);
    if (!nd) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!nd->get_swapchain_images)
        nd->get_swapchain_images = (PFN_vkGetSwapchainImagesKHR)dev_gdpa(dev)(dev, "vkGetSwapchainImagesKHR");
    if (!nd->get_swapchain_images) return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkResult r = nd->get_swapchain_images(dev, sc, count, images);
    if (r == VK_SUCCESS && images != NULL) {
        Chain *c = chain_for(sc);
        if (c) {
            if (*count > LSL_MAX_IMAGES) {
                static int warned;
                if (!warned) {
                    warned = 1;
                    lsl_log("scale: a swapchain reports %u images; only the first "
                            "%d are tracked, so presents of the others stay at "
                            "the application's resolution", *count, LSL_MAX_IMAGES);
                }
            }
            c->image_count = *count < LSL_MAX_IMAGES ? *count : LSL_MAX_IMAGES;
            memcpy(c->images, images, c->image_count * sizeof(VkImage));
            c->have_images = 1;
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL lsl_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
    g_present_count++;

    Chain *c = NULL;
    uint32_t index = 0;
    if (pi && pi->swapchainCount > 0 && pi->pSwapchains && pi->pImageIndices) {
        c = chain_for(pi->pSwapchains[0]);
        index = pi->pImageIndices[0];
        if (c) c->used = ++g_chain_clock;
        if (pi->swapchainCount > 1) {
            /* One present can name several swapchains.  Only the first is
             * upscaled: the others would each need their own engine pass, and
             * nothing here has been seen to use more than one. */
            static int warned_many;
            if (!warned_many) {
                warned_many = 1;
                lsl_log("scale: a present names %u swapchains; only the first is "
                        "upscaled", pi->swapchainCount);
            }
        }
    }

    const int have_frame = c && c->have_images && index < c->image_count;

    if (g_present_count <= 3 || (g_present_count % 120) == 0) {
        lsl_log("present #%llu: chain %p (device %p, engine %p), images %u, index %u, "
                "upscale %d, upscaled so far %llu",
                (unsigned long long)g_present_count, (void *)c,
                c ? (void *)c->dev : NULL, (void *)lsl_upscale_device(),
                c ? c->image_count : 0u, index, c ? c->upscale : 0,
                (unsigned long long)g_upscaled);
    }

    if (have_frame && c->upscale && c->dev == lsl_upscale_device()) {
        if (lsl_upscale_run(queue, family_of_queue(queue), c->images[index],
                            lsl_env_str("LSL_LAYER_DUMP")))
            g_upscaled++;
    } else if (c && c->upscale && !c->have_images) {
        /* The image count was asked for but never the handles, so there is no
         * image to upscale from.  Legal but unusual: say so once rather than
         * presenting at the application's resolution in silence. */
        static int warned_no_images;
        if (!warned_no_images) {
            warned_no_images = 1;
            lsl_log("scale: this swapchain's images were never fetched, so it is "
                    "presented at the application's resolution");
        }
    } else if (c && c->upscale && c->dev != lsl_upscale_device()) {
        /* The layer keeps one engine, bound to the device whose swapchain was
         * created last.  A second live device presenting from its own swapchain
         * would need a second engine, which is the single-device limit noted
         * at the top of this file. */
        static int complained;
        if (!complained) {
            complained = 1;
            lsl_log("scale: this swapchain belongs to a different device than the "
                    "engine was built for; presenting it unscaled");
        }
    } else if (c && scaling() && g_present_count == 1) {
        lsl_log("scale: this swapchain is not upscaled (it was created before, or "
                "at the surface size)");
    }

    if (capture_enabled() && c) {
        if (capture_wanted() && capture_frame(queue, c, index)) g_captured++;
    } else if (!c && g_present_count <= 3) {
        lsl_log("present #%llu: unknown swapchain (not created through this layer)",
                (unsigned long long)g_present_count);
    }

    PFN_vkQueuePresentKHR present_fn = NULL;
    NextDevice *nd = NULL;
    if (c && c->dev) nd = dev_next(c->dev);
    if (nd && nd->present) present_fn = nd->present;
    else if (nd) {
        nd->present = (PFN_vkQueuePresentKHR)dev_gdpa(c->dev)(c->dev, "vkQueuePresentKHR");
        present_fn = nd->present;
    }
    if (!present_fn && g_dev_count > 0) {
        /* Fall back to the first known device's chain. */
        present_fn = (PFN_vkQueuePresentKHR)g_devs[0].gdpa(g_devs[0].dev, "vkQueuePresentKHR");
    }
    if (!present_fn) {
        lsl_log("vkQueuePresentKHR: the next layer/ICD has no present entry point");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return present_fn(queue, pi);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsl_GetDeviceProcAddr(VkDevice dev, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsl_GetInstanceProcAddr(VkInstance inst,
                                                                const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)lsl_CreateInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)lsl_EnumeratePhysicalDevices;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)lsl_CreateDevice;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)lsl_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)lsl_GetDeviceProcAddr;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return (PFN_vkVoidFunction)lsl_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
        return (PFN_vkVoidFunction)lsl_GetPhysicalDeviceSurfaceCapabilities2KHR;
    return g_next_gipa ? g_next_gipa(inst, name) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsl_GetDeviceProcAddr(VkDevice dev, const char *name) {
    if (!name) return NULL;
    if (!g_next_gdpa) return NULL;
    if (!strcmp(name, "vkQueuePresentKHR")) {
        return (PFN_vkVoidFunction)lsl_QueuePresentKHR;
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        return (PFN_vkVoidFunction)lsl_CreateSwapchainKHR;
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
        return (PFN_vkVoidFunction)lsl_DestroySwapchainKHR;
    }
    if (!strcmp(name, "vkGetSwapchainImagesKHR")) {
        return (PFN_vkVoidFunction)lsl_GetSwapchainImagesKHR;
    }
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)lsl_DestroyDevice;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)lsl_GetDeviceQueue;
    if (!strcmp(name, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)lsl_GetDeviceQueue2;
    return g_next_gdpa(dev, name);
}

/* ---------------------------------------------------- exported entry points */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    return lsl_GetInstanceProcAddr(inst, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    return lsl_GetDeviceProcAddr(dev, name);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p) {
    if (!p || p->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = lsl_GetInstanceProcAddr;
    p->pfnGetDeviceProcAddr = lsl_GetDeviceProcAddr;
    return VK_SUCCESS;
}
