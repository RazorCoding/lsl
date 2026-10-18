#include "lsl_vk.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace lsl {
namespace {

[[noreturn]] void fail(const std::string& what, VkResult r) {
    throw std::runtime_error(what + ": VkResult " + std::to_string(int(r)));
}

#define LSL_VK_CHECK(expr, what)                                  \
    do {                                                          \
        VkResult lsl_r_ = (expr);                                 \
        if (lsl_r_ != VK_SUCCESS) fail((what), lsl_r_);           \
    } while (0)

}  // namespace

std::vector<DeviceInfo> enumerate(VkInstance inst) {
    uint32_t n = 0;
    LSL_VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr),
                 "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> raw(n);
    if (n == 0) return {};
    LSL_VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, raw.data()),
                 "vkEnumeratePhysicalDevices");

    std::vector<DeviceInfo> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        DeviceInfo d;
        d.index = i;
        vkGetPhysicalDeviceProperties(raw[i], &d.props);
        vkGetPhysicalDeviceMemoryProperties(raw[i], &d.mem);

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(raw[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(raw[i], &qn, qprops.data());
        for (uint32_t q = 0; q < qn; ++q) {
            if (qprops[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                d.queueFamily = q;
                d.timestampValidBits = qprops[q].timestampValidBits;
                break;
            }
        }
        for (uint32_t h = 0; h < d.mem.memoryHeapCount; ++h) {
            if (d.mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                d.deviceLocalBytes += d.mem.memoryHeaps[h].size;
        }
        out.push_back(d);
    }
    return out;
}

uint32_t pickIndex(const std::vector<DeviceInfo>& devs, std::optional<int> want) {
    if (devs.empty()) throw std::runtime_error("no Vulkan devices visible");
    if (want) {
        if (*want < 0 || size_t(*want) >= devs.size())
            throw std::runtime_error("--device " + std::to_string(*want) +
                                     " out of range (0.." +
                                     std::to_string(devs.size() - 1) + ")");
        return uint32_t(*want);
    }
    // Prefer discrete, then integrated, then whatever exists.
    const VkPhysicalDeviceType pref[] = {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                         VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                         VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                         VK_PHYSICAL_DEVICE_TYPE_CPU};
    for (VkPhysicalDeviceType t : pref)
        for (const auto& d : devs)
            if (d.props.deviceType == t && d.queueFamily != UINT32_MAX)
                return d.index;
    for (const auto& d : devs)
        if (d.queueFamily != UINT32_MAX) return d.index;
    throw std::runtime_error("no device has a compute queue");
}

Context::Context(std::optional<int> want) {
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "lsl";
    app.pEngineName        = "lsl";
    app.apiVersion         = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    LSL_VK_CHECK(vkCreateInstance(&ici, nullptr, &inst_), "vkCreateInstance");

    auto devs = enumerate(inst_);
    uint32_t idx = pickIndex(devs, want);
    info_ = devs[idx];
    phys_ = [&] {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst_, &n, nullptr);
        std::vector<VkPhysicalDevice> raw(n);
        vkEnumeratePhysicalDevices(inst_, &n, raw.data());
        return raw[idx];
    }();

    if (info_.queueFamily == UINT32_MAX)
        throw std::runtime_error("device has no compute queue: " +
                                 std::string(info_.props.deviceName));

    // synchronization2 must be *enabled*, not merely available. Everything this
    // code uses for barriers, submission and timestamps is the sync2 flavour
    // (vkCmdPipelineBarrier2 / vkQueueSubmit2 / vkCmdWriteTimestamp2), and all
    // three are invalid without the feature. RADV and the NVIDIA driver both
    // accept them anyway, so this went unnoticed until the validation layers
    // were switched on -- which is precisely the kind of silent-UB that only
    // validation catches.
    VkPhysicalDeviceSynchronization2Features sync2{};
    sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &sync2;
    vkGetPhysicalDeviceFeatures2(phys_, &f2);

    std::vector<const char*> devExt;
    if (info_.props.apiVersion < VK_API_VERSION_1_3) {
        // Below 1.3 the feature is only reachable through the extension, which
        // has to be enabled for the feature struct to be legal.
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, exts.data());
        bool have = false;
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
                have = true;
        if (!have)
            throw std::runtime_error(std::string("device has neither Vulkan 1.3 nor "
                                                 "VK_KHR_synchronization2, which this "
                                                 "code requires: ") +
                                     info_.props.deviceName);
        devExt.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    if (!sync2.synchronization2)
        throw std::runtime_error(std::string("device does not support synchronization2, "
                                             "which this code requires: ") +
                                 info_.props.deviceName);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = info_.queueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &sync2;  // synchronization2 = VK_TRUE, verified just above
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = uint32_t(devExt.size());
    dci.ppEnabledExtensionNames = devExt.empty() ? nullptr : devExt.data();
    LSL_VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &dev_), "vkCreateDevice");

    vkGetDeviceQueue(dev_, info_.queueFamily, 0, &queue_);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
}

Context::~Context() {
    if (dev_)   vkDeviceWaitIdle(dev_);
    if (dev_)   vkDestroyDevice(dev_, nullptr);
    if (inst_)  vkDestroyInstance(inst_, nullptr);
}

uint32_t Context::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps_.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    throw std::runtime_error("no memory type matching requested flags");
}

Context::Buffer Context::createBuffer(VkDeviceSize size, const void* initial) {
    return createBufferEx(size,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          initial);
}

Context::Buffer Context::createBufferEx(VkDeviceSize size, VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags props,
                                        const void* initial) {
    Buffer b;
    b.size = size;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    LSL_VK_CHECK(vkCreateBuffer(dev_, &bci, nullptr, &b.buf), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    LSL_VK_CHECK(vkAllocateMemory(dev_, &ai, nullptr, &b.mem), "vkAllocateMemory");
    LSL_VK_CHECK(vkBindBufferMemory(dev_, b.buf, b.mem, 0), "vkBindBufferMemory");

    if (initial) {
        void* p = map(b);
        std::memcpy(p, initial, size_t(size));
        unmap(b);
    }
    return b;
}

void Context::destroyBuffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = Buffer{};
}

void* Context::map(Buffer& b) {
    void* p = nullptr;
    LSL_VK_CHECK(vkMapMemory(dev_, b.mem, 0, b.size, 0, &p), "vkMapMemory");
    return p;
}

void Context::unmap(Buffer& b) { vkUnmapMemory(dev_, b.mem); }

void Context::invalidate(Buffer& b, VkDeviceSize size) {
    VkMappedMemoryRange r{};
    r.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = b.mem;
    r.offset = 0;
    r.size   = size;
    // HOST_COHERENT memory needs no explicit invalidate; call kept for the
    // non-coherent path this benchmark may exercise later.
    (void)r;
}

VkShaderModule Context::createShader(const uint32_t* words, size_t wordCount) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = wordCount * sizeof(uint32_t);
    ci.pCode    = words;
    VkShaderModule m = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateShaderModule(dev_, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

VkPipelineLayout Context::createLayout(VkPushConstantRange pcr,
                                       VkDescriptorSetLayout setLayout) const {
    VkPipelineLayoutCreateInfo ci{};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = setLayout ? 1u : 0u;
    ci.pSetLayouts            = setLayout ? &setLayout : nullptr;
    ci.pushConstantRangeCount = pcr.size ? 1u : 0u;
    ci.pPushConstantRanges    = pcr.size ? &pcr : nullptr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreatePipelineLayout(dev_, &ci, nullptr, &l), "vkCreatePipelineLayout");
    return l;
}

VkDescriptorSetLayout Context::createSetLayout() const {
    return createSetLayoutN(2);
}

VkDescriptorSetLayout Context::createSetLayoutN(uint32_t count) const {
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);
    for (uint32_t i = 0; i < count; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = count;
    ci.pBindings    = bindings.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateDescriptorSetLayout(dev_, &ci, nullptr, &l),
                 "vkCreateDescriptorSetLayout");
    return l;
}

VkPipeline Context::createCompute(VkPipelineLayout layout, VkShaderModule sh) const {
    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = sh;
    ci.stage.pName       = "main";
    ci.layout            = layout;
    ci.basePipelineHandle = VK_NULL_HANDLE;
    ci.basePipelineIndex  = -1;
    VkPipeline p = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p),
                 "vkCreateComputePipelines");
    return p;
}

VkDescriptorPool Context::createPool() const {
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sz.descriptorCount = 64;
    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets       = 64;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &sz;
    VkDescriptorPool p = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateDescriptorPool(dev_, &ci, nullptr, &p), "vkCreateDescriptorPool");
    return p;
}

VkDescriptorSet Context::allocSet(VkDescriptorPool pool, VkDescriptorSetLayout lay,
                                  Buffer src, Buffer dst) const {
    const Buffer bufs[2] = {src, dst};
    return allocSetN(pool, lay, bufs, 2);
}

VkDescriptorSet Context::allocSetN(VkDescriptorPool pool, VkDescriptorSetLayout lay,
                                   const Buffer* buffers, uint32_t count) const {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &lay;
    VkDescriptorSet set = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkAllocateDescriptorSets(dev_, &ai, &set), "vkAllocateDescriptorSets");

    std::vector<VkDescriptorBufferInfo> infos(count);
    std::vector<VkWriteDescriptorSet> writes(count);
    for (uint32_t i = 0; i < count; ++i) {
        infos[i].buffer = buffers[i].buf;
        infos[i].offset = 0;
        infos[i].range  = buffers[i].size;
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(dev_, count, writes.data(), 0, nullptr);
    return set;
}

VkCommandPool Context::createCmdPool() const {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = info_.queueFamily;
    VkCommandPool p = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateCommandPool(dev_, &ci, nullptr, &p), "vkCreateCommandPool");
    return p;
}

VkCommandBuffer Context::allocCmd(VkCommandPool pool) const {
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &c), "vkAllocateCommandBuffers");
    return c;
}

VkQueryPool Context::createQueries(uint32_t count) const {
    VkQueryPoolCreateInfo ci{};
    ci.sType   = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = count;
    VkQueryPool p = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateQueryPool(dev_, &ci, nullptr, &p), "vkCreateQueryPool");
    return p;
}

void Context::submitAndWait(VkCommandBuffer cmd) {
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateFence(dev_, &fci, nullptr, &fence), "vkCreateFence");

    VkCommandBufferSubmitInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbs.commandBuffer = cmd;

    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbs;

    LSL_VK_CHECK(vkQueueSubmit2(queue_, 1, &si, fence), "vkQueueSubmit2");
    LSL_VK_CHECK(vkWaitForFences(dev_, 1, &fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences");
    vkDestroyFence(dev_, fence, nullptr);
}

double Context::submitTimed(VkCommandBuffer cmd, VkCommandPool /*pool*/, VkQueryPool queries) {
    // The caller's command buffer must already contain:
    //   vkCmdResetQueryPool(cmd, queries, 0, 2)
    //   vkCmdWriteTimestamp(cmd, COMPUTE, queries, 0)   // before dispatch
    //   ... dispatch ...
    //   vkCmdWriteTimestamp(cmd, COMPUTE, queries, 1)   // after dispatch
    // Timestamps therefore bracket exactly the work we care about, and the
    // buffer is never reset here (resetting would discard those commands).
    if (!timestampsUsable()) {
        submitAndWait(cmd);
        return 0.0;
    }

    VkCommandBufferSubmitInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbs.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbs;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    LSL_VK_CHECK(vkCreateFence(dev_, &fci, nullptr, &fence), "vkCreateFence");
    LSL_VK_CHECK(vkQueueSubmit2(queue_, 1, &si, fence), "vkQueueSubmit2");
    LSL_VK_CHECK(vkWaitForFences(dev_, 1, &fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences");
    vkDestroyFence(dev_, fence, nullptr);
    (void)queries;

    uint64_t t[2] = {0, 0};
    VkResult r = vkGetQueryPoolResults(dev_, queries, 0, 2, sizeof(t), t,
                                       sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return 0.0;
    const double ns = double(t[1] - t[0]) * double(timestampPeriodNs());
    return ns / 1.0e6;
}

}  // namespace lsl
