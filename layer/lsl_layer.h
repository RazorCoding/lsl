/*
 * lsl_layer.h -- state shared between the layer's dispatch (lsl_layer.c) and its
 * upscaling engine (lsl_upscale.c).
 *
 * The whole layer is C on purpose: it is loaded into arbitrary processes and
 * must not pull in a C++ runtime.
 *
 * The split is deliberate.  The dispatch only knows how to find entry points
 * and when to call into the engine; everything about how a frame is upscaled
 * lives in lsl_upscale.c.  Frame generation, when it arrives, becomes a third
 * file next to these.
 */
#pragma once

#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ logging */

void lsl_log(const char *fmt, ...);

/* ------------------------------------------------------------------ env ---- */

/* Non-empty and not "0".  Cached by the caller, not here: these are read on the
 * present path, which is hot. */
int lsl_env_flag(const char *name);
/* NULL when unset or empty. */
const char *lsl_env_str(const char *name);

/* ------------------------------------------------------- instance and device */

extern VkInstance g_instance;
extern VkPhysicalDevice g_physdev;
extern VkDevice g_device;
extern VkPhysicalDeviceMemoryProperties g_memprops;
extern int g_have_memprops;

/* ---------------------------------------------------------------------------
 * Entry points from the next layer in the chain.
 *
 * Resolved once, at device creation, from the pointer the loader handed us.
 * Deliberately *not* the loader's exported entry points: those re-enter the
 * chain at the top, and a layer cannot hand them its own handles -- the loader
 * rejects a VkPhysicalDevice that was created inside a layer ("Invalid
 * physicalDevice"), and the object handles are no different.  Tried; it aborts.
 *
 * Commands recorded here can still be validated: with the Khronos validation
 * layer loaded *underneath* this one it sees every call, provided the command
 * buffers carry the device's dispatch word (see lsl_stamp_command_buffers) --
 * the loader stamps it for the application, and a layer that allocates its own
 * has to do the same or VVL aborts with "The VkDevice dispatch handle was not
 * found".  tools/upscale-check.sh runs that configuration.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* command buffers */
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkResetCommandBuffer reset_command_buffer;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image;
    PFN_vkCmdCopyBuffer cmd_copy_buffer;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets;
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdDispatch cmd_dispatch;
    /* synchronization */
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkWaitForFences wait_for_fences;
    PFN_vkQueueSubmit queue_submit;
    /* memory */
    PFN_vkMapMemory map_memory;
    PFN_vkUnmapMemory unmap_memory;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindBufferMemory bind_buffer_memory;
    /* compute pipelines */
    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout;
    PFN_vkCreatePipelineLayout create_pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateDescriptorPool create_descriptor_pool;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets;
    PFN_vkUpdateDescriptorSets update_descriptor_sets;
} LslDevFns;

/* Fill `out` with the next-chain device entry points resolved for `dev`.  Each
 * device has its own chain, so a table is only valid for the device it was
 * resolved for.  Hold it next to the device it belongs to: in a process with
 * two live devices, calling one device's objects through the other's table is
 * not a wrong answer, it is invalid (VUID-...-commonparent). */
void lsl_resolve_device_fns(VkDevice dev, LslDevFns *out);

/* Stamp command buffers allocated by the layer with the device dispatch word
 * the loader would have stamped them with.  Call after every
 * vkAllocateCommandBuffers. */
void lsl_stamp_command_buffers(VkDevice dev, uint32_t count,
                               const VkCommandBuffer *cbs);

/* The next layer's vkGetInstanceProcAddr, for the few instance-level queries the
 * engine needs (queue family properties, surface support). */
extern PFN_vkGetInstanceProcAddr g_next_gipa;

/* The physical device `dev` was created from, or VK_NULL_HANDLE if unknown. */
VkPhysicalDevice lsl_pd_of_dev(VkDevice dev);

/* Resolve an instance-level entry point through the instance that owns `pd`.
 *
 * Two live instances in one process are normal -- another implicit layer
 * creates its own -- and an instance-level call has to be dispatched through
 * the instance its arguments came from.  Dispatching a surface or a physical
 * device through the other instance is not a wrong answer, it is a validation
 * error (VUID-vkGetPhysicalDeviceSurfaceSupportKHR-commonparent). */
PFN_vkVoidFunction lsl_pd_proc(VkPhysicalDevice pd, const char *name);

/* Pick a memory type matching `want` and not matching `avoid`. */
int lsl_pick_memory_type(uint32_t bits, VkMemoryPropertyFlags want,
                         VkMemoryPropertyFlags avoid, uint32_t *out);

/* ---------------------------------------------------------------------------
 * The upscaling engine.
 *
 * The whole feature is: make the application render smaller than the surface by
 * lying about the surface capabilities, then push its frame back to full size
 * through the chosen method before the present lands.  Every method starts with
 * the same present-time pipeline -- copy the frame into a buffer, upscale it
 * with shaders/scale.comp, optionally refine at destination resolution -- so a
 * method is only a choice of which passes run in between.  See the pass plan in
 * lsl_upscale.c's up_record().
 * ------------------------------------------------------------------------- */

typedef struct {
    VkFormat format;      /* swapchain format */
    uint32_t real_w, real_h;  /* the size the swapchain really is (N x M) */
    uint32_t app_w, app_h;    /* the size the application thinks it is (W x H) */
    float sharpness;          /* RCAS strength, in stops; only fsr1rcas uses it */
    uint32_t mode;            /* an LSL_MODE_* value, or LSL_MODE_AUTO */
} LslUpscaleShape;

/* Build (or rebuild, if `shape` or the device changed) the compute pipeline and
 * buffers.  Returns 0 -- and leaves nothing half-built -- if anything is
 * unsupported.  The engine's objects belong to `dev`, which the present path
 * re-checks through lsl_upscale_device(). */
int lsl_upscale_begin(const LslUpscaleShape *shape, VkSurfaceKHR surface, VkDevice dev);

/* The device the engine is currently built for, or VK_NULL_HANDLE.  A swapchain
 * created on a different device must not be upscaled through it. */
VkDevice lsl_upscale_device(void);

/* Queue family the engine records for; the caller must submit on a queue from
 * this family or the image ownership transfers would be wrong. */
uint32_t lsl_upscale_family(void);

/* Record and submit the upscale for one presented image.  Returns 1 if the
 * frame was upscaled.  `dump_prefix` (or NULL) writes a one-off pair of PPMs
 * for offline comparison -- see tools/upscale-check.sh. */
int lsl_upscale_run(VkQueue queue, uint32_t family, VkImage image,
                    const char *dump_prefix);

void lsl_upscale_end(void);

/* Wait for the frames already handed to the queue.  Called before the images a
 * submitted dispatch reads can go away -- vkDestroySwapchainKHR. */
void lsl_upscale_drain(void);

/* ------------------------------------------------------------------ formats */

int lsl_format_supported(VkFormat f);
int lsl_format_is_16f(VkFormat f);
int lsl_format_is_bgra(VkFormat f);
uint32_t lsl_format_texel_bytes(VkFormat f);
