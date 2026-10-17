#pragma once
// lsl - minimal Vulkan scaffolding for the headless scaler benchmark.
// Deliberately small: instance, device, compute queue, buffers, pipeline,
// submit with GPU timestamps. No layer/swapchain code lives here.

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lsl {

struct DeviceInfo {
    uint32_t                     index = 0;
    VkPhysicalDeviceProperties   props{};
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t                     queueFamily = UINT32_MAX;
    uint32_t                     timestampValidBits = 0;
    VkDeviceSize                 deviceLocalBytes = 0;
};

// Enumerates physical devices of the instance. queueFamily is filled with the
// first COMPUTE-capable family, or UINT32_MAX when none exists.
std::vector<DeviceInfo> enumerate(VkInstance inst);

// Picks the requested index, else a discrete GPU, else the first usable one.
uint32_t pickIndex(const std::vector<DeviceInfo>& devs, std::optional<int> want);

class Context {
  public:
    explicit Context(std::optional<int> want = std::nullopt);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkDevice             device()       const { return dev_; }
    VkPhysicalDevice     physical()     const { return phys_; }
    const DeviceInfo&    info()         const { return info_; }
    uint32_t             queueFamily()  const { return info_.queueFamily; }
    float                timestampPeriodNs() const { return info_.props.limits.timestampPeriod; }
    bool                 timestampsUsable() const { return info_.timestampValidBits > 0; }

    struct Buffer {
        VkBuffer       buf  = VK_NULL_HANDLE;
        VkDeviceMemory mem  = VK_NULL_HANDLE;
        VkDeviceSize   size = 0;
    };

    Buffer createBuffer(VkDeviceSize size, const void* initial = nullptr);
    // Generalized variant: explicit usage and memory properties, e.g. a
    // DEVICE_LOCAL store fed by a HOST_VISIBLE staging buffer.
    Buffer createBufferEx(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props, const void* initial = nullptr);
    void   destroyBuffer(Buffer& b);
    void*  map(Buffer& b);
    void   unmap(Buffer& b);
    void   invalidate(Buffer& b, VkDeviceSize size);

    // Records nothing; submits `cmd` and waits on a fence. Exposed so callers
    // can run one-off upload/download command buffers.
    void     submitAndWait(VkCommandBuffer cmd);

    VkShaderModule        createShader(const uint32_t* words, size_t wordCount) const;
    // The pipeline layout MUST reference the descriptor set layout, otherwise
    // vkCmdBindDescriptorSets(firstSet=0) is out of range and the shader runs
    // with no valid descriptors (writes silently land nowhere).
    VkPipelineLayout      createLayout(VkPushConstantRange pcr,
                                       VkDescriptorSetLayout setLayout) const;
    VkDescriptorSetLayout createSetLayout() const;
    VkPipeline            createCompute(VkPipelineLayout layout, VkShaderModule sh) const;
    VkDescriptorPool      createPool() const;
    VkDescriptorSet       allocSet(VkDescriptorPool pool, VkDescriptorSetLayout lay,
                                   Buffer src, Buffer dst) const;
    VkCommandPool         createCmdPool() const;
    VkCommandBuffer       allocCmd(VkCommandPool pool) const;
    VkQueryPool           createQueries(uint32_t count) const;

    // Submits `cmd` and waits on a fence, returning the GPU-measured elapsed
    // milliseconds from the timestamps recorded *inside* that command buffer.
    // Returns 0.0 when timestamps are unavailable on this queue.
    double submitTimed(VkCommandBuffer cmd, VkCommandPool pool, VkQueryPool queries);

  private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

    VkInstance                      inst_  = VK_NULL_HANDLE;
    VkPhysicalDevice                phys_  = VK_NULL_HANDLE;
    VkDevice                        dev_   = VK_NULL_HANDLE;
    VkQueue                         queue_ = VK_NULL_HANDLE;
    DeviceInfo                      info_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
};

// Push constant block; must match shaders/scale.comp and shaders/rcas.comp.
// `sharpness` is an RCAS strength in stops (0 = maximum) and is only read by the
// RCAS pass; the scaler shader ignores it.
struct Push {
    uint32_t srcW, srcH, dstW, dstH;
    uint32_t mode;
    float    sharpness;
    uint32_t pad0, pad1;
};
static_assert(sizeof(Push) == 32, "push constant block changed size");

}  // namespace lsl
