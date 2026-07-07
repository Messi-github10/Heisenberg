//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <vulkan/vulkan.hpp>

#include <memory>
#include <vector>

#ifdef VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace heisenberg {
namespace render {

class VulkanContext {
public:
    static VulkanContext& instance();

    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&)                 = delete;
    VulkanContext& operator=(VulkanContext&&)      = delete;

    VkInstance       vkInstance()        const;
    VkPhysicalDevice vkPhysicalDevice()  const;
    VkDevice         vkDevice()          const;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const;

    uint32_t  graphicsQueueFamily()            const;
    vk::Queue graphicsQueue()                  const;
    const std::vector<const char*>& deviceExtensions() const;

#ifdef VK_USE_PLATFORM_WIN32_KHR
    VkSurfaceKHR createSurface(HWND hwnd) const;
#endif

private:
    VulkanContext();

    void initVolkAndDispatcher();
    void createInstance();
    void selectPhysicalDevice();
    void createDevice();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
