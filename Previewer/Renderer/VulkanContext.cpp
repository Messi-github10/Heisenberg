//
// Created by NiceFold on 2026/6/30.
//

#include "VulkanContext.hpp"
#include <volk.h>
#include <Utiles/Logger.hpp>
#include <stdexcept>

// Vulkan-Hpp 动态分发全局存储
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace heisenberg {
namespace renderer {

struct VulkanContext::Impl {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    bool loaderInitialized = false;
    bool instanceLoaded   = false;
    bool deviceLoaded     = false;
};

VulkanContext& VulkanContext::instance() {
    static VulkanContext ctx;
    return ctx;
}

VulkanContext::VulkanContext()
    : impl_(std::make_unique<Impl>()) {}

void VulkanContext::initVolkLoader() {
    if (impl_->loaderInitialized) return;

    VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        throw std::runtime_error("volkInitialize() failed — Vulkan loader not found");
    }

    impl_->vkGetInstanceProcAddr = vkGetInstanceProcAddr;

    impl_->loaderInitialized = true;
    LOG_INFO("VulkanContext: Volk loader initialized");
}

void VulkanContext::initInstance(VkInstance qtInstance) {
    if (!impl_->loaderInitialized) {
        LOG_ERROR("VulkanContext: initInstance called before initVolkLoader");
        return;
    }
    if (!qtInstance) {
        LOG_ERROR("VulkanContext: initInstance called with null instance");
        return;
    }

    // 将 Qt 的 VkInstance 加载到 Volk
    volkLoadInstance(qtInstance);

    // 将 Qt 的 VkInstance 加载到 Vulkan-Hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(qtInstance, impl_->vkGetInstanceProcAddr);

    impl_->instanceLoaded = true;
    LOG_INFO("VulkanContext: Qt VkInstance loaded into Volk + Hpp");
}

void VulkanContext::initDevice(VkDevice qtDevice) {
    if (!impl_->instanceLoaded) {
        LOG_ERROR("VulkanContext: initDevice called before initInstance");
        return;
    }
    if (!qtDevice) {
        LOG_ERROR("VulkanContext: initDevice called with null device");
        return;
    }

    volkLoadDevice(qtDevice);

    impl_->deviceLoaded = true;
    LOG_INFO("VulkanContext: Qt VkDevice loaded into Volk");
}

PFN_vkGetInstanceProcAddr VulkanContext::getInstanceProcAddr() const {
    return impl_->vkGetInstanceProcAddr;
}

} // namespace renderer
} // namespace heisenberg
