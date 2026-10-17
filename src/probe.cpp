// lsl-probe: enumerate every visible Vulkan device and report what will
// actually be used. Also flags the two environment traps on this host:
//   * VK_LOADER_DRIVERS_DISABLE hides the NVIDIA ICD from the whole session
//   * only one ICD visible means any "benchmark" is really the iGPU

#include "lsl_vk.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* typeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:            return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:          return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:             return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:                     return "cpu";
        default:                                              return "other";
    }
}

}  // namespace

int main() {
    const char* disable = std::getenv("VK_LOADER_DRIVERS_DISABLE");
    if (disable && *disable) {
        std::cout << "WARNING: VK_LOADER_DRIVERS_DISABLE=" << disable << "\n"
                  << "         The NVIDIA Vulkan ICD is hidden for this process; the\n"
                  << "         GTX 1650 will not appear. On this host Hyprland sets\n"
                  << "         this globally (hyprland.lua:26) and Steam removes it:\n"
                  << "           env -u VK_LOADER_DRIVERS_DISABLE prime-run <program>\n\n";
    }
    for (const char* var : {"VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_LOADER_DRIVERS_SELECT"}) {
        const char* v = std::getenv(var);
        if (v && *v) std::cout << "note: " << var << "=" << v << "\n";
    }

    VkApplicationInfo app{};
    app.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed: " << r << "\n";
        return 2;
    }

    auto devs = lsl::enumerate(inst);
    if (devs.empty()) {
        std::cerr << "no Vulkan devices visible.\n";
        vkDestroyInstance(inst, nullptr);
        return 1;
    }

    const uint32_t chosen = lsl::pickIndex(devs, std::nullopt);

    std::cout << devs.size() << " Vulkan device(s) visible"
              << (devs.size() > 1 ? " (both ICDs reachable)" : "") << "\n\n";

    for (const auto& d : devs) {
        const uint32_t hi = d.props.apiVersion >> 22;
        const uint32_t lo = (d.props.apiVersion >> 12) & 0x3ff;
        std::cout << (d.index == chosen ? "> " : "  ")
                  << "GPU" << d.index << "  " << d.props.deviceName << "\n"
                  << "      type        : " << typeName(d.props.deviceType) << "\n"
                  << "      vendor      : 0x" << std::hex << d.props.vendorID
                  << "  device 0x" << d.props.deviceID << std::dec
                  << "  (driver " << d.props.driverVersion << ")\n"
                  << "      api         : Vulkan " << hi << "." << lo << "\n"
                  << "      compute qf  : " << d.queueFamily << "\n"
                  << "      timestamp   : period " << d.props.limits.timestampPeriod
                  << " ns, valid bits " << d.timestampValidBits << "\n"
                  << "      deviceLocal : "
                  << (d.deviceLocalBytes / (1024 * 1024)) << " MB\n"
                  << "      maxWG count : "
                  << d.props.limits.maxComputeWorkGroupCount[0] << " x "
                  << d.props.limits.maxComputeWorkGroupCount[1] << " x "
                  << d.props.limits.maxComputeWorkGroupCount[2] << "\n\n";
    }

    std::cout << "default selection: GPU" << chosen << " ("
              << devs[chosen].props.deviceName << ")\n";

    if (devs.size() == 1 && !disable)
        std::cout << "note: only one device visible; set/unset VK_LOADER_DRIVERS_DISABLE\n"
                  << "      and re-run to confirm both ICDs are reachable.\n";

    vkDestroyInstance(inst, nullptr);
    return 0;
}
