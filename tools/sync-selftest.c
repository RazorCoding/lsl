/*
 * sync-selftest.c -- proves *synchronization* validation is really active.
 *
 * Core validation has a self-test (validation-selftest.c); this is the exact
 * analogue for hazards, and it exists because "no hazard reported" is only
 * meaningful if a known hazard would have been reported.
 *
 * The program deliberately records two vkCmdFillBuffer calls targeting the same
 * buffer, with no barrier in between, in one command buffer, and submits them.
 * That is an unambiguous write-after-write / write-racing-write hazard: the
 * second fill may run before, after, or interleaved with the first.  No driver
 * flags it; only synchronization validation does.
 *
 * Expected with synchronization validation on:
 *     SYNC-HAZARD-WRITE-RACING-WRITE   (or WRITE-AFTER-WRITE)
 * Expected with it off:
 *     nothing but SELFTEST_RAN.
 *
 * It doubles as the negative control: with the layer disabled it must print no
 * hazard, or the message above is noise.
 */
#include <stdio.h>
#include <vulkan/vulkan.h>

static void fail(const char *what, VkResult r) {
    fprintf(stderr, "sync-selftest: %s -> VkResult %d\n", what, (int)r);
}

int main(void) {
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "lsl-sync-selftest";
    app.applicationVersion = 0;
    app.apiVersion = VK_API_VERSION_1_1;

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        fail("vkCreateInstance", r);
        printf("SELFTEST_SKIPPED no instance\n");
        return 0;
    }

    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (nd == 0) {
        printf("SELFTEST_SKIPPED no devices\n");
        return 0;
    }
    VkPhysicalDevice pd[8];
    if (nd > 8) nd = 8;
    vkEnumeratePhysicalDevices(inst, &nd, pd);

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
    r = vkCreateDevice(pd[0], &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        fail("vkCreateDevice", r);
        printf("SELFTEST_SKIPPED no device\n");
        return 0;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    r = vkCreateBuffer(dev, &bci, NULL, &buf);
    if (r != VK_SUCCESS) {
        fail("vkCreateBuffer", r);
        printf("SELFTEST_SKIPPED no buffer\n");
        return 0;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);

    uint32_t mt = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd[0], &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mt = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) {
        fail("vkAllocateMemory", r);
        printf("SELFTEST_SKIPPED no memory\n");
        return 0;
    }
    vkBindBufferMemory(dev, buf, mem, 0);

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) {
        printf("SELFTEST_SKIPPED no command pool\n");
        return 0;
    }

    VkCommandBufferAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cb);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    /* The hazard: two writes to the same range, no barrier in between. */
    vkCmdFillBuffer(cb, buf, 0, 4096, 0x11111111u);
    vkCmdFillBuffer(cb, buf, 0, 4096, 0x22222222u);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        fail("vkQueueSubmit", r);
    }
    vkQueueWaitIdle(queue);

    printf("SELFTEST_RAN\n");
    return 0;
}
