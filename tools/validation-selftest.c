/*
 * validation-selftest.c -- proves the Vulkan validation layer is really active.
 *
 * Deliberately commits two API violations and then prints SELFTEST_RAN.  The
 * violations are chosen because drivers return VK_SUCCESS for both, so the
 * program looks healthy without validation: core validation is the only thing
 * that reports them.
 *
 * Callers must therefore require that the run produces a "Validation Error"
 * *and* the SELFTEST_RAN marker.  Seeing the marker without an error means the
 * layer is installed but not actually validating -- a silent failure mode the
 * loader does not report unless VK_LOADER_DEBUG is set.
 *
 * Built ad hoc by tools/validation-check.sh; not part of the library.
 */

#include <vulkan/vulkan.h>
#include <stdio.h>

int main(void)
{
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "lsl-validation-selftest";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) {
        printf("SELFTEST_SKIPPED no instance\n");
        return 0;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n == 0) {
        printf("SELFTEST_SKIPPED no devices\n");
        return 0;
    }
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pd);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {0};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = 0;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;

    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd[0], &dci, NULL, &dev) != VK_SUCCESS) {
        printf("SELFTEST_SKIPPED no device\n");
        return 0;
    }

    /* Violation 1: a buffer must have non-zero usage. */
    VkBufferCreateInfo b1 = {0};
    b1.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    b1.size = 1024;
    b1.usage = 0;
    b1.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer tmp = VK_NULL_HANDLE;
    (void)vkCreateBuffer(dev, &b1, NULL, &tmp);

    /* Violation 2: sType must be the matching structure type. */
    VkBufferCreateInfo b2 = {0};
    b2.sType = (VkStructureType)0x7fffffff;
    b2.size = 1024;
    b2.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    b2.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    tmp = VK_NULL_HANDLE;
    (void)vkCreateBuffer(dev, &b2, NULL, &tmp);

    printf("SELFTEST_RAN\n");
    return 0;
}
