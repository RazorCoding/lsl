/*
 * resize-check.c -- the layer's swapchain path when a swapchain is recreated.
 *
 * tools/upscale-check.sh presents one swapchain that never changes size, so the
 * paths a game exercises on an alt-tab, a resolution change or a window drag --
 * vkCreateSwapchainKHR with oldSwapchain set, the previous swapchain destroyed
 * right after, a different extent each time, frames still on the queue while all
 * of that happens -- are not covered by it.
 *
 * This is the smallest program that covers them: an XCB window and a swapchain
 * that is only ever filled with a fixed pattern, resized in a loop.  Every call
 * is one a normal application makes, so a failure here is the layer's:
 *
 *   - it asks the surface how big it is and uses that extent, which is what the
 *     layer's lie is for (a swapchain created at the surface size is not
 *     upscaled, because there is nothing to upscale);
 *   - it renders the *application's* extent into the top-left of whatever image
 *     it gets, then leaves it in PRESENT_SRC_KHR;
 *   - it keeps two frames in flight, so the create of the next swapchain -- and
 *     the only-extent rebuild the layer does inside it -- happens with a dispatch
 *     the layer submitted at present time possibly still on the queue;
 *   - it recreates once without changing the size, which is what an application
 *     does when it recovers from VK_ERROR_OUT_OF_DATE_KHR or switches present
 *     mode.
 *
 * The evidence that matters is the layer's own log (one "- upscaling" per
 * swapchain, "resized to ... (pipelines kept)" for the extent-only path, the
 * dumped PPMs) and the validation layer's silence; tools/resize-check.sh reads
 * all of it.  This program only asserts what it can see for itself: that no call
 * came back wrong, and that the extent it was told to use is the one the layer
 * promised.
 *
 * Built ad hoc by tools/resize-check.sh; not part of the library.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The XCB surface extension, whose types are only declared when this is set
 * before vulkan.h. */
#define VK_USE_PLATFORM_XCB_KHR 1
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

/* Two frames in flight, like a game. */
#define FRAMES 2
/* Upper bound on what this test ever asks for, so the pattern buffer and the
 * image array are fixed size. */
#define MAX_W 1024u
#define MAX_H 640u

static int g_failures;

static void fail(const char *what) {
    fprintf(stderr, "resize-check: FAIL: %s\n", what);
    g_failures++;
}

static void ok(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "resize-check: FAIL: %s -> %d\n", what, (int)r);
        g_failures++;
    }
}

/* ------------------------------------------------------------ XCB and Vulkan */

static xcb_connection_t *g_conn;
static xcb_screen_t *g_screen;
static xcb_window_t g_win;

static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_queue;
static VkSurfaceKHR g_surface;

static VkCommandPool g_pool;
static VkCommandBuffer g_cmd[FRAMES];
static VkFence g_fence[FRAMES];
/* The semaphore vkAcquireNextImageKHR signals and the submission waits on is
 * per frame: the fence the frame is bound to covers the wait. */
static VkSemaphore g_avail[FRAMES];
/* The semaphore the submission signals and the present waits on is per
 * *swapchain image* instead: only the acquire of that image proves the
 * presentation operation that consumed it has finished, so a single semaphore
 * shared by two frames in flight would be signalled while a present still
 * holds it.  That is exactly what the validation layer's semaphore-reuse
 * guidance (and VUID-vkQueueSubmit-pSignalSemaphores-00067) asks for. */
#define LSL_MAX_IMAGES 32
static VkSemaphore g_done[LSL_MAX_IMAGES];
static uint32_t g_frame;

static VkSwapchainKHR g_swap;
static VkFormat g_format;
static uint32_t g_lie_w, g_lie_h;         /* what the application is told */
static VkImage g_images[LSL_MAX_IMAGES];
static uint32_t g_image_count;
static uint32_t g_win_w, g_win_h;         /* what the window manager was asked */

static VkBuffer g_pattern;
static VkDeviceMemory g_pattern_mem;
static float g_scale = 1.0f;

/* A window manager is free to choose a window's size -- a tiling one always
 * does, which turns a "resize" into a no-op.  The ICCCM has the escape hatch:
 * a window whose minimum and maximum size are the same is a fixed-size window,
 * tiling window managers float it, and then a configure request is honoured.
 * The property is written by hand rather than through xcb-icccm so the test
 * needs nothing beyond xcb; the atom numbers are the predefined ones from
 * X11's atom table (WM_NORMAL_HINTS and WM_SIZE_HINTS), which is what a
 * window manager reads. */
static void set_fixed_size_hint(uint32_t w, uint32_t h) {
    enum { PMinSizeFlag = 16, PMaxSizeFlag = 32 };
    uint32_t fields[9];   /* XSizeHints up to max_height */
    memset(fields, 0, sizeof(fields));
    fields[0] = PMinSizeFlag | PMaxSizeFlag;
    fields[5] = w;   /* min_width  */
    fields[6] = h;   /* min_height */
    fields[7] = w;   /* max_width  */
    fields[8] = h;   /* max_height */
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_win, XCB_ATOM_WM_NORMAL_HINTS,
                        XCB_ATOM_WM_SIZE_HINTS, 32, 9, fields);
    xcb_flush(g_conn);
}

static int open_window(uint32_t w, uint32_t h) {
    int screen_num = 0;
    g_conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(g_conn)) return 0;

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(g_conn));
    for (int i = 0; i < screen_num && it.rem; ++i) xcb_screen_next(&it);
    if (!it.rem) return 0;
    g_screen = it.data;

    g_win = xcb_generate_id(g_conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {g_screen->black_pixel, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(g_conn, XCB_COPY_FROM_PARENT, g_win, g_screen->root, 0, 0,
                      (uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      g_screen->root_visual, mask, values);
    set_fixed_size_hint(w, h);
    xcb_map_window(g_conn, g_win);
    xcb_flush(g_conn);
    g_win_w = w;
    g_win_h = h;
    return 1;
}

static void resize_window(uint32_t w, uint32_t h) {
    set_fixed_size_hint(w, h);
    uint32_t dims[2] = {w, h};
    xcb_configure_window(g_conn, g_win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dims);
    xcb_flush(g_conn);
    g_win_w = w;
    g_win_h = h;
}

/* The window manager and the compositor handle the resize asynchronously, so a
 * real application asks the surface what its size is now and often asks more
 * than once.  Without the layer the answer is the window size; with it, the
 * answer is the window size divided by the scale.  Either is accepted, and which
 * one came back says whether the lie is in effect. */
static int wait_for_surface(uint32_t *out_w, uint32_t *out_h) {
    const uint32_t lie_w = (uint32_t)((double)g_win_w / g_scale);
    const uint32_t lie_h = (uint32_t)((double)g_win_h / g_scale);
    for (int attempt = 0; attempt < 500; ++attempt) {
        xcb_flush(g_conn);
        VkSurfaceCapabilitiesKHR caps;
        memset(&caps, 0, sizeof(caps));
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surface, &caps) != VK_SUCCESS) {
            usleep(10000);
            continue;
        }
        if (caps.currentExtent.width == UINT32_MAX) {   /* no size from the surface */
            *out_w = lie_w;
            *out_h = lie_h;
            return 1;
        }
        if (caps.currentExtent.width == g_win_w && caps.currentExtent.height == g_win_h) {
            *out_w = g_win_w;
            *out_h = g_win_h;
            return 1;
        }
        if (caps.currentExtent.width == lie_w && caps.currentExtent.height == lie_h) {
            *out_w = lie_w;
            *out_h = lie_h;
            return 1;
        }
        usleep(10000);
    }
    return 0;
}

static int pick_format(VkFormat *out) {
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &n, NULL) != VK_SUCCESS || n == 0)
        return 0;
    VkSurfaceFormatKHR *formats = malloc(n * sizeof(*formats));
    if (!formats) return 0;
    VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &n, formats);
    if (r != VK_SUCCESS) {
        free(formats);
        return 0;
    }
    int found = 0;
    /* The same preference as most applications: 8-bit sRGB or UNORM, either
     * channel order -- the layer handles both, and the check script compares in
     * the shader's channel order. */
    const VkFormat wanted[] = {VK_FORMAT_B8G8R8A8_UNORM,     VK_FORMAT_B8G8R8A8_SRGB,
                               VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_R8G8B8A8_SRGB};
    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]) && !found; ++i)
        for (uint32_t j = 0; j < n; ++j)
            if (formats[j].format == wanted[i]) {
                *out = formats[j].format;
                found = 1;
                break;
            }
    if (!found && n == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        *out = VK_FORMAT_B8G8R8A8_UNORM;   /* "whatever you like", a legacy answer */
        found = 1;
    }
    free(formats);
    return found;
}

static int create_instance(void) {
    const char *exts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "lsl-resize-check";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VkResult r = vkCreateInstance(&ci, NULL, &g_inst);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "resize-check: vkCreateInstance -> %d\n", (int)r);
        return 0;
    }

    VkXcbSurfaceCreateInfoKHR sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    sci.connection = g_conn;
    sci.window = g_win;
    r = vkCreateXcbSurfaceKHR(g_inst, &sci, NULL, &g_surface);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "resize-check: vkCreateXcbSurfaceKHR -> %d\n", (int)r);
        return 0;
    }
    return 1;
}

/* The first device with one queue family that can both draw and present, which
 * is also the family the layer's engine will pick.  A machine where those are
 * two families needs the ownership transfer the prototype declines, so this test
 * skips instead of failing. */
static int create_device(void) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(g_inst, &count, NULL) != VK_SUCCESS || count == 0) return 0;
    VkPhysicalDevice *pds = malloc(count * sizeof(*pds));
    if (!pds) return 0;
    if (vkEnumeratePhysicalDevices(g_inst, &count, pds) != VK_SUCCESS) {
        free(pds);
        return 0;
    }

    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i) {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &n, NULL);
        if (!n) continue;
        VkQueueFamilyProperties *props = malloc(n * sizeof(*props));
        if (!props) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &n, props);
        for (uint32_t f = 0; f < n; ++f) {
            if (!(props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!(props[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], f, g_surface, &present) != VK_SUCCESS)
                continue;
            if (!present) continue;
            g_pd = pds[i];
            family = f;
            break;
        }
        free(props);
    }
    free(pds);
    if (family == UINT32_MAX) return 0;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char *exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    ok(vkCreateDevice(g_pd, &dci, NULL, &g_dev), "vkCreateDevice");
    if (!g_dev) return 0;
    vkGetDeviceQueue(g_dev, family, 0, &g_queue);
    return 1;
}

/* ------------------------------------------------------------------- pattern */

/* A fixed function of (x, y): a horizontal and a vertical ramp, a fine
 * checkerboard and a few bright dots -- a smooth gradient for EASU to
 * interpolate and hard edges for RCAS to sharpen, all of it identical no matter
 * which extent the swapchain ends up with. */
static void fill_pattern(uint8_t *p, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *px = p + ((size_t)y * stride + x) * 4;
            px[0] = (uint8_t)((x * 255u) / (w ? w : 1));
            px[1] = (uint8_t)((y * 255u) / (h ? h : 1));
            px[2] = (uint8_t)((((x >> 3) ^ (y >> 3)) & 1u) ? 230 : 20);
            px[3] = 255;
            if ((x + y) % 97u == 0) {
                px[0] = px[1] = px[2] = 255;
            }
        }
    }
}

static int make_pattern(void) {
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)MAX_W * MAX_H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ok(vkCreateBuffer(g_dev, &bci, NULL, &g_pattern), "vkCreateBuffer(pattern)");
    if (!g_pattern) return 0;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, g_pattern, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        fail("no host visible, coherent memory for the pattern");
        return 0;
    }

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    ok(vkAllocateMemory(g_dev, &mai, NULL, &g_pattern_mem), "vkAllocateMemory(pattern)");
    if (!g_pattern_mem) return 0;
    ok(vkBindBufferMemory(g_dev, g_pattern, g_pattern_mem, 0), "vkBindBufferMemory(pattern)");

    void *mapped = NULL;
    if (vkMapMemory(g_dev, g_pattern_mem, 0, bci.size, 0, &mapped) != VK_SUCCESS) {
        fail("vkMapMemory(pattern)");
        return 0;
    }
    fill_pattern(mapped, MAX_W, MAX_H, MAX_W);
    vkUnmapMemory(g_dev, g_pattern_mem);
    return 1;
}

/* ------------------------------------------------------------------ swapchain */

static int make_swapchain(VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps;
    memset(&caps, 0, sizeof(caps));
    ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surface, &caps), "surface capabilities");
    if (caps.maxImageExtent.width == 0 || caps.maxImageExtent.height == 0) {
        fail("the surface reports no usable extent");
        return 0;
    }

    uint32_t w, h;
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    } else {
        w = g_win_w;
        h = g_win_h;
        if (w < caps.minImageExtent.width) w = caps.minImageExtent.width;
        if (w > caps.maxImageExtent.width) w = caps.maxImageExtent.width;
        if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
        if (h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;
    }
    if (w < caps.minImageExtent.width || h < caps.minImageExtent.height ||
        w > caps.maxImageExtent.width || h > caps.maxImageExtent.height) {
        fprintf(stderr, "resize-check: extent %ux%u is outside the surface's %ux%u..%ux%u\n", w,
                h, caps.minImageExtent.width, caps.minImageExtent.height,
                caps.maxImageExtent.width, caps.maxImageExtent.height);
        g_failures++;
        return 0;
    }

    /* Two, or the minimum plus one, whichever is larger: enough that a frame can
     * be on the queue while another is acquired, and never above the driver's
     * maximum. */
    uint32_t images = caps.minImageCount + 1;
    if (images < 2) images = 2;
    if (caps.maxImageCount > 0 && images > caps.maxImageCount) images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = g_surface;
    ci.minImageCount = images;
    ci.imageFormat = g_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = w;
    ci.imageExtent.height = h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;

    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &swap);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "resize-check: vkCreateSwapchainKHR(%ux%u, old %p) -> %d\n", w, h,
                (void *)old, (int)r);
        g_failures++;
        return 0;
    }

    g_swap = swap;
    g_lie_w = w;
    g_lie_h = h;

    uint32_t n = 0;
    ok(vkGetSwapchainImagesKHR(g_dev, g_swap, &n, NULL), "swapchain image count");
    if (n > sizeof(g_images) / sizeof(g_images[0])) n = sizeof(g_images) / sizeof(g_images[0]);
    ok(vkGetSwapchainImagesKHR(g_dev, g_swap, &n, g_images), "swapchain images");
    g_image_count = n;
    if (g_image_count < 2) {
        fail("a swapchain with fewer than two images");
        return 0;
    }

    printf("resize-check: swapchain %ux%u (window %ux%u), %u images, format %d, old %p\n", w, h,
           g_win_w, g_win_h, g_image_count, (int)g_format, (void *)old);
    return 1;
}

static int present_frame(void) {
    const uint32_t i = g_frame++ % FRAMES;
    ok(vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, 10000000000ull), "vkWaitForFences(frame)");
    ok(vkResetFences(g_dev, 1, &g_fence[i]), "vkResetFences(frame)");

    uint32_t index = 0;
    VkResult ar = vkAcquireNextImageKHR(g_dev, g_swap, 10000000000ull, g_avail[i],
                                       VK_NULL_HANDLE, &index);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "resize-check: vkAcquireNextImageKHR -> %d\n", (int)ar);
        g_failures++;
        return 0;
    }
    if (index >= g_image_count) {
        fail("the image index acquired is outside the swapchain");
        return 0;
    }

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ok(vkBeginCommandBuffer(g_cmd[i], &bi), "vkBeginCommandBuffer");

    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g_images[index];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    /* The presentation engine released the image, so its contents are undefined
     * and can be discarded -- which also means the whole image is written below. */
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    /* Only the application's own extent: that is what a game draws and the only
     * part of the image the layer ever looks at. */
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferRowLength = MAX_W;
    region.bufferImageHeight = MAX_H;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = g_lie_w;
    region.imageExtent.height = g_lie_h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g_cmd[i], g_pattern, g_images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g_cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    ok(vkEndCommandBuffer(g_cmd[i]), "vkEndCommandBuffer");

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_avail[i];
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd[i];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_done[index];
    ok(vkQueueSubmit(g_queue, 1, &si, g_fence[i]), "vkQueueSubmit");

    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &index;
    VkResult pr = vkQueuePresentKHR(g_queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "resize-check: vkQueuePresentKHR -> %d\n", (int)pr);
        g_failures++;
        return 0;
    }
    return 1;
}

/* Destroying a swapchain requires every use of its images to have completed,
 * and the presentation of the *last* image is a use the application's own fences
 * do not cover -- the presentation engine's completion is only observable with
 * VK_KHR_present_wait, which this driver does not have.  Waiting for the device
 * is what closes that gap, so it is what the application does here.  It is a
 * wait *after* the new swapchain was created, which is the moment that matters:
 * the layer's rebuild in vkCreateSwapchainKHR runs with the frames of the
 * previous swapchain still possibly on the queue, and that is what its wait-slot
 * path is for. */
static void destroy_swapchain(VkSwapchainKHR swap) {
    if (!swap) return;
    ok(vkDeviceWaitIdle(g_dev), "vkDeviceWaitIdle(before destroy)");
    vkDestroySwapchainKHR(g_dev, swap, NULL);
}

/* ---------------------------------------------------------------------- main */

static int setup_frames(void) {
    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ok(vkCreateCommandPool(g_dev, &cpci, NULL, &g_pool), "vkCreateCommandPool");
    if (!g_pool) return 0;

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = FRAMES;
    ok(vkAllocateCommandBuffers(g_dev, &cbai, g_cmd), "vkAllocateCommandBuffers");

    for (uint32_t i = 0; i < FRAMES; ++i) {
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        ok(vkCreateFence(g_dev, &fci, NULL, &g_fence[i]), "vkCreateFence");

        VkSemaphoreCreateInfo sci;
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ok(vkCreateSemaphore(g_dev, &sci, NULL, &g_avail[i]), "vkCreateSemaphore(avail)");
    }
    for (uint32_t i = 0; i < LSL_MAX_IMAGES; ++i) {
        VkSemaphoreCreateInfo sci;
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ok(vkCreateSemaphore(g_dev, &sci, NULL, &g_done[i]), "vkCreateSemaphore(done)");
    }
    return 1;
}

static void teardown(void) {
    if (g_dev) vkDeviceWaitIdle(g_dev);
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        if (g_fence[i]) vkDestroyFence(g_dev, g_fence[i], NULL);
        if (g_avail[i]) vkDestroySemaphore(g_dev, g_avail[i], NULL);
    }
    for (uint32_t i = 0; i < LSL_MAX_IMAGES; ++i)
        if (g_done[i]) vkDestroySemaphore(g_dev, g_done[i], NULL);
    if (g_pool) vkDestroyCommandPool(g_dev, g_pool, NULL);
    if (g_pattern) vkDestroyBuffer(g_dev, g_pattern, NULL);
    if (g_pattern_mem) vkFreeMemory(g_dev, g_pattern_mem, NULL);
    if (g_dev) vkDestroyDevice(g_dev, NULL);
    if (g_surface) vkDestroySurfaceKHR(g_inst, g_surface, NULL);
    if (g_inst) vkDestroyInstance(g_inst, NULL);
    if (g_conn) {
        xcb_destroy_window(g_conn, g_win);
        xcb_disconnect(g_conn);
    }
}

int main(void) {
    const char *scale = getenv("LSL_LAYER_SCALE");
    if (scale && *scale) {
        g_scale = (float)atof(scale);
        if (!(g_scale > 1.0f)) g_scale = 1.0f;
    }

    if (!open_window(700, 440)) {
        printf("resize-check: no X display\n");
        return 77;
    }
    if (!create_instance()) return 77;   /* no surface support: nothing to test */
    if (!create_device()) {
        printf("resize-check: no single queue family can both draw and present\n");
        return 77;
    }
    if (!pick_format(&g_format)) {
        printf("resize-check: the surface offers no 8-bit format\n");
        return 77;
    }
    if (!setup_frames() || !make_pattern()) {
        teardown();
        return 1;
    }

    /* The first swapchain is created but never presented from: the layer dumps
     * the images of the first frame it upscales, and that frame should be one
     * that came after a recreate, because that is the case upscale-check.sh
     * cannot reach. */
    if (!make_swapchain(VK_NULL_HANDLE)) {
        teardown();
        return 1;
    }

    /* Every step resizes, asks the surface for the new size, creates the next
     * swapchain from the previous one, and only then destroys the previous one --
     * the order a game uses, and the order that leaves a frame the layer
     * submitted on the queue while the swapchain underneath it changes. */
    static const uint32_t steps[][2] = {
        {800, 500}, {1024, 640}, {640, 400}, {900, 560}, {760, 480}, {640, 400},
    };
    const int presents_per_step = 3;

    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        resize_window(steps[s][0], steps[s][1]);

        uint32_t w = 0, h = 0;
        if (!wait_for_surface(&w, &h)) {
            fail("the surface never named the resized extent");
            teardown();
            return 1;
        }
        if (w != g_lie_w || h != g_lie_h) {
            printf("resize-check: the surface now says %ux%u (was %ux%u)\n", w, h, g_lie_w,
                   g_lie_h);
        }

        VkSwapchainKHR old = g_swap;
        if (!make_swapchain(old)) {
            teardown();
            return 1;
        }
        destroy_swapchain(old);

        for (int p = 0; p < presents_per_step; ++p)
            if (!present_frame()) {
                teardown();
                return 1;
            }
    }

    /* One recreate with the size unchanged: what an application does when it
     * recovers from VK_ERROR_OUT_OF_DATE_KHR or switches present mode. */
    {
        VkSwapchainKHR old = g_swap;
        if (!make_swapchain(old)) {
            teardown();
            return 1;
        }
        destroy_swapchain(old);
        for (int p = 0; p < presents_per_step; ++p)
            if (!present_frame()) {
                teardown();
                return 1;
            }
    }

    teardown();

    if (g_failures) {
        fprintf(stderr, "resize-check: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("resize-check: %zu recreates, %d presents, all calls returned success\n",
           sizeof(steps) / sizeof(steps[0]) + 1, g_frame);
    printf("RESIZE_CHECK_OK\n");
    return 0;
}
