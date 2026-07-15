//
// Created by NiceFold on 2026/6/30.
//

#include "VulkanContext.hpp"
#include <volk.h>
#include <Utiles/Logger.hpp>
#include <stdexcept>
#include <cstring>
#include <set>
#include <vector>

// Vulkan-Hpp 动态分发全局存储
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace heisenberg {
namespace renderer {

struct VulkanContext::Impl {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    bool loaderInitialized = false;

    vk::Instance       vkInstance;
    vk::PhysicalDevice vkPhysDevice;
    vk::Device         vkDevice;
    uint32_t           graphicsQF = 0;
    vk::Queue          graphicsQueue;

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
};

VulkanContext& VulkanContext::instance() {
    static VulkanContext ctx;
    return ctx;
}

VulkanContext::VulkanContext()
    : impl_(std::make_unique<Impl>()) {}

VulkanContext::~VulkanContext() {
    if (impl_->vkDevice) {
        impl_->vkDevice.destroy();
    }
    if (impl_->debugMessenger && impl_->vkInstance) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            impl_->vkInstance.getProcAddr("vkDestroyDebugUtilsMessengerEXT"));
        if (fn) {
            fn(impl_->vkInstance, impl_->debugMessenger, nullptr);
        }
    }
    if (impl_->vkInstance) {
        impl_->vkInstance.destroy();
    }
}

void VulkanContext::initLoader() {
    if (impl_->loaderInitialized) return;

    VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        throw std::runtime_error("volkInitialize() failed — Vulkan loader not found");
    }

    impl_->vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    impl_->loaderInitialized = true;
    LOG_INFO("VulkanContext: Volk loader initialized");
}

std::vector<const char*> VulkanContext::requiredInstanceExtensions() const {
    std::vector<const char*> exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
#ifdef _DEBUG
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    return exts;
}

void VulkanContext::createInstance(bool enableValidation) {
    if (!impl_->loaderInitialized) {
        throw std::runtime_error("VulkanContext: initLoader() must be called first");
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkGetInstanceProcAddr);

    auto exts = requiredInstanceExtensions();

    vk::ApplicationInfo appInfo;
    appInfo.pApplicationName   = "Heisenberg";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "Heisenberg Engine";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    vk::InstanceCreateInfo createInfo;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    createInfo.ppEnabledExtensionNames = exts.data();

    // Validation layers
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (enableValidation) {
        createInfo.enabledLayerCount   = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }

    impl_->vkInstance = vk::createInstance(createInfo);
    if (!impl_->vkInstance) {
        throw std::runtime_error("vkCreateInstance() failed");
    }

    volkLoadInstance(impl_->vkInstance);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkInstance, impl_->vkGetInstanceProcAddr);

    LOG_INFO("VulkanContext: VkInstance created");
}

std::vector<const char*> VulkanContext::requiredDeviceExtensions() const {
    return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
}

std::optional<uint32_t> VulkanContext::findGraphicsQueueFamily(vk::PhysicalDevice physDev) const {
    auto queueFamilies = physDev.getQueueFamilyProperties();
    for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilies.size()); ++i) {
        if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            return i;
        }
    }
    return std::nullopt;
}

void VulkanContext::createDevice() {
    if (!impl_->vkInstance) {
        throw std::runtime_error("VulkanContext: createInstance() must be called first");
    }

    auto physDevices = impl_->vkInstance.enumeratePhysicalDevices();
    if (physDevices.empty()) {
        throw std::runtime_error("No Vulkan-capable physical devices found");
    }

    vk::PhysicalDevice chosen = physDevices[0];
    for (auto& pd : physDevices) {
        auto props = pd.getProperties();
        if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            chosen = pd;
            break;
        }
    }
    impl_->vkPhysDevice = chosen;

    auto props = impl_->vkPhysDevice.getProperties();
    LOG_INFO("VulkanContext: selected physical device — {}", props.deviceName.data());

    auto qf = findGraphicsQueueFamily(impl_->vkPhysDevice);
    if (!qf) {
        throw std::runtime_error("No graphics queue family found");
    }
    impl_->graphicsQF = *qf;

    auto availableExts = impl_->vkPhysDevice.enumerateDeviceExtensionProperties();
    LOG_INFO("VulkanContext: {} device extensions available", availableExts.size());

    auto devExts = requiredDeviceExtensions();
    for (auto* ext : devExts) {
        bool found = false;
        for (auto& avail : availableExts) {
            if (std::strcmp(avail.extensionName.data(), ext) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR("VulkanContext: required device extension '{}' not available!", ext);
        }
    }
    LOG_INFO("VulkanContext: enabling {} device extensions", devExts.size());

    float queuePriority = 1.0f;
    vk::DeviceQueueCreateInfo qCreateInfo;
    qCreateInfo.queueFamilyIndex = impl_->graphicsQF;
    qCreateInfo.queueCount       = 1;
    qCreateInfo.pQueuePriorities = &queuePriority;

    vk::PhysicalDeviceVulkan11Features vk11Features;
    vk11Features.shaderDrawParameters = VK_TRUE;

    vk::PhysicalDeviceVulkan12Features vk12Features;
    vk12Features.timelineSemaphore = VK_TRUE;

    vk::DeviceCreateInfo deviceInfo;
    deviceInfo.queueCreateInfoCount    = 1;
    deviceInfo.pQueueCreateInfos       = &qCreateInfo;
    deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    deviceInfo.ppEnabledExtensionNames = devExts.data();
    deviceInfo.pNext                   = &vk11Features;
    vk11Features.pNext                 = &vk12Features;

    impl_->vkDevice = impl_->vkPhysDevice.createDevice(deviceInfo);

    volkLoadDevice(impl_->vkDevice);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkInstance, impl_->vkDevice);

    impl_->graphicsQueue = impl_->vkDevice.getQueue(impl_->graphicsQF, 0);

    LOG_INFO("VulkanContext: VkDevice created — QF={}", impl_->graphicsQF);
}

PFN_vkGetInstanceProcAddr VulkanContext::getInstanceProcAddr() const {
    return impl_->vkGetInstanceProcAddr;
}

vk::Instance VulkanContext::vkInstance() const {
    return impl_->vkInstance;
}

vk::PhysicalDevice VulkanContext::physicalDevice() const {
    return impl_->vkPhysDevice;
}

vk::Device VulkanContext::device() const {
    return impl_->vkDevice;
}

uint32_t VulkanContext::graphicsQueueFamily() const {
    return impl_->graphicsQF;
}

vk::Queue VulkanContext::graphicsQueue() const {
    return impl_->graphicsQueue;
}

} // namespace renderer
} // namespace heisenberg
