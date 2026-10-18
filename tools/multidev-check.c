/*
 * multidev-check.c -- the layer's device lifetime when one process holds
 * several instances and devices.
 *
 * A Proton game is not one VkDevice.  SnowRunner's log is the shape that broke
 * the layer: several VkInstance/VkDevice pairs in one process, a device
 * destroyed while the next is made, and a helper instance another implicit
 * layer (lsfg-vk) creates for itself.  When the layer resolved the next
 * chain's entry points into a single global cache, the second device
 * overwrote the first's pointers and the first's presentation was dispatched
 * through the wrong chain -- a crash before any swapchain existed.
 *
 * This is that shape with the window removed, which is all it takes to
 * exercise the layer's per-instance/per-device bookkeeping:
 *
 *   - two live instances (the application's and a helper's);
 *   - four devices, queues fetched through the layer so each call resolves
 *     that device's own next-chain GetDeviceProcAddr;
 *   - a device destroyed while another is still alive (the moment the old
 *     global cache went stale);
 *   - vkGetDeviceProcAddr asked for a function on every device, so the
 *     per-device path is the one being walked.
 *
 * The evidence is the layer's own log and the validation layer's silence;
 * tools/multidev-check.sh reads both.  This program only asserts what it can
 * see for itself: every call returned VK_SUCCESS and nothing crashed.
 *
 * Built ad hoc by tools/multidev-check.sh; not part of the library.
 */

#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void ok(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "multidev-check: FAIL: %s -> %d\n", what, (int)r);
        g_failures++;
    }
}

/* One instance plus the device-creation facts of its first physical device. */
typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    uint32_t family;
} Env;

static int find_family(VkPhysicalDevice pd) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (!n) return -1;
    VkQueueFamilyProperties *q = calloc(n, sizeof(*q));
    if (!q) return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    int family = -1;
    for (uint32_t i = 0; i < n; ++i)
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            family = (int)i;
            break;
        }
    free(q);
    return family;
}

static int environment(Env *e, const char *name) {
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = name;
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ok(vkCreateInstance(&ci, NULL, &e->inst), "vkCreateInstance");
    if (!e->inst) return 0;

    uint32_t n = 0;
    ok(vkEnumeratePhysicalDevices(e->inst, &n, NULL), "vkEnumeratePhysicalDevices(count)");
    if (!n) return 0;
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    if (!pds) return 0;
    ok(vkEnumeratePhysicalDevices(e->inst, &n, pds), "vkEnumeratePhysicalDevices");
    e->pd = pds[0];
    free(pds);

    const int family = find_family(e->pd);
    if (family < 0) {
        fprintf(stderr, "multidev-check: no graphics+compute queue family\n");
        return 0;
    }
    e->family = (uint32_t)family;
    return 1;
}

static VkDevice make_device(Env *e, VkQueue *queue) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = e->family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev = VK_NULL_HANDLE;
    ok(vkCreateDevice(e->pd, &dci, NULL, &dev), "vkCreateDevice");
    if (!dev) return VK_NULL_HANDLE;

    /* Both go through the device's own dispatch table: the queue fetch is
     * resolved per device by the layer, and asking for the present entry point
     * is what the application does before it presents. */
    vkGetDeviceQueue(dev, e->family, 0, queue);
    if (!vkGetDeviceProcAddr(dev, "vkQueuePresentKHR")) {
        fprintf(stderr, "multidev-check: FAIL: no vkQueuePresentKHR on a device\n");
        g_failures++;
    }
    if (queue && *queue)
        ok(vkQueueWaitIdle(*queue), "vkQueueWaitIdle");
    return dev;
}

static void drop(VkDevice dev) {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
}

static void drop_env(Env *e) {
    if (e->inst) vkDestroyInstance(e->inst, NULL);
    memset(e, 0, sizeof(*e));
}

int main(void) {
    Env app = {0}, helper = {0};
    if (!environment(&app, "lsl-multidev-check")) return 77;
    if (!environment(&helper, "lsl-multidev-check helper")) {
        vkDestroyInstance(app.inst, NULL);
        return 1;
    }

    /* The application's first device and the helper's, alive together. */
    VkQueue q0 = VK_NULL_HANDLE, qh = VK_NULL_HANDLE;
    VkDevice d0 = make_device(&app, &q0);
    VkDevice dh = make_device(&helper, &qh);

    /* The application drops its first device while the helper stays alive --
     * with the old, single global cache this is where the live next-chain
     * pointers went stale. */
    vkDeviceWaitIdle(d0);
    vkDestroyDevice(d0, NULL);

    /* Rebuilt on the same instance, two alive at once, each with its own
     * queue and next-chain pointers. */
    VkQueue q1 = VK_NULL_HANDLE, q2 = VK_NULL_HANDLE;
    VkDevice d1 = make_device(&app, &q1);
    VkDevice d2 = make_device(&app, &q2);

    (void)q0;
    (void)qh;

    vkDeviceWaitIdle(dh);
    vkDestroyDevice(dh, NULL);
    if (d1) {
        vkDeviceWaitIdle(d1);
        vkDestroyDevice(d1, NULL);
    }
    if (d2) {
        vkDeviceWaitIdle(d2);
        vkDestroyDevice(d2, NULL);
    }

    if (helper.inst) vkDestroyInstance(helper.inst, NULL);
    if (app.inst) vkDestroyInstance(app.inst, NULL);

    if (g_failures) {
        fprintf(stderr, "multidev-check: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("multidev-check: 2 instances, 4 devices, all calls returned success\n");
    printf("MULTIDEV_CHECK_OK\n");
    return 0;
}
