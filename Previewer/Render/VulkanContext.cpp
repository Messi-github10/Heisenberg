//
// Created by NiceFold on 2026/6/30.
//

#include "VulkanContext.hpp"

#include <volk.h>
#include <Utiles/Logger.hpp>

#include <stdexcept>
#include <cstring>

#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif

// Vulkan-Hpp 动态分发全局存储
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace heisenberg {
namespace render {

struct VulkanContext::Impl {
    vk::UniqueInstance   instance;
    vk::PhysicalDevice   physicalDevice;
    vk::UniqueDevice     device;

    uint32_t  graphicsQF = 0;
    vk::Queue graphicsQueue;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

    std::vector<const char*> deviceExts;
};

VulkanContext& VulkanContext::instance() {
    static VulkanContext ctx;
    return ctx;
}

VulkanContext::VulkanContext()
    : impl_(std::make_unique<Impl>()) {
    initVolkAndDispatcher();
    createInstance();
    selectPhysicalDevice();
    createDevice();
}

VulkanContext::~VulkanContext() = default;

void VulkanContext::initVolkAndDispatcher() {
    VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        throw std::runtime_error("volkInitialize() failed — Vulkan loader not found");
    }

    impl_->vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkGetInstanceProcAddr);

    LOG_INFO("Vulkan: Volk + Hpp dispatcher initialized");
}

void VulkanContext::createInstance() {
    vk::ApplicationInfo appInfo;
    appInfo.setPApplicationName("Heisenberg");
    appInfo.setApplicationVersion(VK_MAKE_VERSION(1, 0, 0));
    appInfo.setPEngineName("Heisenberg");
    appInfo.setEngineVersion(VK_MAKE_VERSION(1, 0, 0));
    appInfo.setApiVersion(VK_API_VERSION_1_3);

    std::vector<const char*> instanceExts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    std::vector<const char*> layers;
#ifndef NDEBUG
    instanceExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    auto availableLayers = vk::enumerateInstanceLayerProperties();
    for (const auto& l : availableLayers) {
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            LOG_INFO("Vulkan: validation layer enabled");
            break;
        }
    }
    if (layers.empty()) {
        LOG_INFO("Vulkan: validation layer not found — skipping");
    }
#endif

    vk::InstanceCreateInfo ci;
    ci.setPApplicationInfo(&appInfo);
    ci.setEnabledExtensionCount(static_cast<uint32_t>(instanceExts.size()));
    ci.setPpEnabledExtensionNames(instanceExts.data());
    ci.setEnabledLayerCount(static_cast<uint32_t>(layers.size()));
    ci.setPpEnabledLayerNames(layers.data());

    impl_->instance = vk::createInstanceUnique(ci);

    volkLoadInstance(impl_->instance.get());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->instance.get());

    LOG_INFO("Vulkan: Instance created");
}

void VulkanContext::selectPhysicalDevice() {
    auto devices = impl_->instance->enumeratePhysicalDevices();
    if (devices.empty()) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    // 优先独立显卡
    for (auto& dev : devices) {
        auto props = dev.getProperties();
        if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            impl_->physicalDevice = dev;
            LOG_INFO("Vulkan: selected discrete GPU — {}", props.deviceName.data());
            break;
        }
    }

    if (!impl_->physicalDevice) {
        impl_->physicalDevice = devices[0];
        auto props = impl_->physicalDevice.getProperties();
        LOG_INFO("Vulkan: fallback to {} — {}",
                 props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu ? "integrated GPU" : "other",
                 props.deviceName.data());
    }

    auto queueFamilies = impl_->physicalDevice.getQueueFamilyProperties();
    for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
        if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            impl_->graphicsQF = i;
            break;
        }
    }

    LOG_INFO("Vulkan: graphics queue family index = {}", impl_->graphicsQF);
}

void VulkanContext::createDevice() {
    float priority = 1.0f;
    vk::DeviceQueueCreateInfo qci;
    qci.setQueueFamilyIndex(impl_->graphicsQF);
    qci.setQueueCount(1);
    qci.setPQueuePriorities(&priority);

    // Feature 链：Sync2 → Ycbcr → Vk12
    vk::PhysicalDeviceSynchronization2Features sync2Features;
    sync2Features.setSynchronization2(VK_TRUE);

    vk::PhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures;
    ycbcrFeatures.setSamplerYcbcrConversion(VK_TRUE);
    ycbcrFeatures.setPNext(&sync2Features);

    vk::PhysicalDeviceVulkan12Features vk12Features;
    vk12Features.setTimelineSemaphore(VK_TRUE);
    vk12Features.setPNext(&ycbcrFeatures);

    impl_->deviceExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };

    vk::DeviceCreateInfo dci;
    dci.setQueueCreateInfoCount(1);
    dci.setPQueueCreateInfos(&qci);
    dci.setEnabledExtensionCount(static_cast<uint32_t>(impl_->deviceExts.size()));
    dci.setPpEnabledExtensionNames(impl_->deviceExts.data());
    dci.setPNext(&vk12Features);

    impl_->device = impl_->physicalDevice.createDeviceUnique(dci);

    volkLoadDevice(impl_->device.get());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->device.get());

    impl_->graphicsQueue = impl_->device->getQueue(impl_->graphicsQF, 0);

    LOG_INFO("Vulkan: Device created successfully");
}

VkInstance VulkanContext::vkInstance() const {
    return impl_->instance.get();
}

VkPhysicalDevice VulkanContext::vkPhysicalDevice() const {
    return impl_->physicalDevice;
}

VkDevice VulkanContext::vkDevice() const {
    return impl_->device.get();
}

PFN_vkGetInstanceProcAddr VulkanContext::getInstanceProcAddr() const {
    return impl_->vkGetInstanceProcAddr;
}

uint32_t VulkanContext::graphicsQueueFamily() const {
    return impl_->graphicsQF;
}

vk::Queue VulkanContext::graphicsQueue() const {
    return impl_->graphicsQueue;
}

const std::vector<const char*>& VulkanContext::deviceExtensions() const {
    return impl_->deviceExts;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR
VkSurfaceKHR VulkanContext::createSurface(HWND hwnd) const {
    try {
        vk::Win32SurfaceCreateInfoKHR sci;
        sci.setHinstance(GetModuleHandle(nullptr));
        sci.setHwnd(hwnd);

        vk::SurfaceKHR surface = impl_->instance->createWin32SurfaceKHR(sci);
        return static_cast<VkSurfaceKHR>(surface);
    } catch (const vk::SystemError& e) {
        LOG_ERROR("Vulkan: createWin32SurfaceKHR failed — {}", e.what());
        return VK_NULL_HANDLE;
    }
}
#endif

} // namespace render
} // namespace heisenberg
