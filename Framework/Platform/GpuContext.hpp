//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/vulkan.h>
}

namespace heisenberg {
namespace renderer {

struct VulkanResources {
    vk::Instance instance = nullptr;
    vk::PhysicalDevice physDevice = nullptr;
    vk::Device device = nullptr;
    uint32_t graphicsQF = 0;
    vk::Queue graphicsQueue = nullptr;
    PFN_vkGetInstanceProcAddr getProcAddr = nullptr;
    const VkPhysicalDeviceFeatures2* enabledFeatures = nullptr;
    std::vector<const char*> enabledExtensions;
    uint32_t apiVersion = VK_API_VERSION_1_2;
};

class GpuContext : public NonCopy {
public:
    explicit GpuContext(const VulkanResources& vkRes);
    ~GpuContext();

    pl_gpu plGpu() const;
    pl_vulkan plVulkan() const;

private:
    void createLog();
    void importVulkan(const VulkanResources& vkRes);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
