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
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdlib>

// Vulkan-Hpp 动态分发全局存储
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace heisenberg {
namespace renderer {

struct VulkanContext::Impl {
    mutable std::recursive_mutex initializationMutex;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    bool loaderInitialized = false;

    vk::Instance       vkInstance;
    vk::PhysicalDevice vkPhysDevice;
    vk::Device         vkDevice;
    uint32_t           graphicsQF = 0;
    vk::Queue          graphicsQueue;
    uint32_t           computeQF = 0;
    vk::Queue          computeQueue;
    bool               aloneCompute = false;

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    std::atomic<uint64_t> validationErrors{0};
    std::array<uint8_t, VK_LUID_SIZE> deviceLuid{};
    bool deviceLuidValidFlag = false;
    VkPhysicalDeviceFeatures2 enabledFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features enabled11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceSynchronization2Features enabledSync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    std::vector<const char*> enabledExtensions;
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
    std::lock_guard lock(impl_->initializationMutex);
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
        // The imported libplacebo swapchain calls capabilities2 entry points.
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
    };
#ifdef _DEBUG
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    return exts;
}

void VulkanContext::createInstance(bool enableValidation) {
    std::lock_guard lock(impl_->initializationMutex);
    if (impl_->vkInstance) {
        // Raw Vulkan calls in this application use Volk's process-wide dispatch.
        // Never silently reuse this instance after foreign code replaced that
        // dispatch (possibly with an already destroyed instance/device).
        if (volkGetLoadedInstance() != static_cast<VkInstance>(impl_->vkInstance))
            throw std::logic_error("VulkanContext: global Volk instance dispatch was replaced by a foreign instance; use the shared context or isolated dispatch tables");
        return;
    }
    if (!impl_->loaderInitialized) {
        throw std::runtime_error("VulkanContext: initLoader() must be called first");
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkGetInstanceProcAddr);

    const char* layerEnvironment = std::getenv("VK_INSTANCE_LAYERS");
    const bool captureValidation = enableValidation || (layerEnvironment
        && std::strstr(layerEnvironment, "VK_LAYER_KHRONOS_validation"));
    auto exts = requiredInstanceExtensions();
    if (captureValidation && std::none_of(exts.begin(), exts.end(), [](const char* ext) {
            return std::strcmp(ext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        })) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

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

    if (captureValidation) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pUserData = &impl_->validationErrors;
        messenger.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                      VkDebugUtilsMessageTypeFlagsEXT,
                                      const VkDebugUtilsMessengerCallbackDataEXT* data,
                                      void* counter) -> VkBool32 {
            static_cast<std::atomic<uint64_t>*>(counter)->fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("Vulkan validation: {}", data && data->pMessage ? data->pMessage : "Unknown validation error");
            return VK_FALSE;
        };
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            impl_->vkInstance.getProcAddr("vkCreateDebugUtilsMessengerEXT"));
        if (!createMessenger || createMessenger(impl_->vkInstance, &messenger, nullptr, &impl_->debugMessenger) != VK_SUCCESS)
            throw std::runtime_error("Cannot create Vulkan validation error messenger");
    }
    LOG_INFO("VulkanContext: VkInstance created");
}

std::vector<const char*> VulkanContext::requiredDeviceExtensions() const {
    return {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        // Prebuilt libplacebo swapchain uses stage NONE in legacy barriers.
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    };
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

std::optional<uint32_t> VulkanContext::findAloneCompute(vk::PhysicalDevice physDev) const {
    auto queueFamilies = physDev.getQueueFamilyProperties();
    for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilies.size()); ++i) {
        bool hasCompute  = static_cast<bool>(queueFamilies[i].queueFlags & vk::QueueFlagBits::eCompute);
        bool hasGraphics = static_cast<bool>(queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics);
        if (hasCompute && !hasGraphics) {
            return i;
        }
    }
    return std::nullopt;
}

void VulkanContext::createDevice() {
    std::lock_guard lock(impl_->initializationMutex);
    if (impl_->vkDevice) {
        if (volkGetLoadedInstance() != static_cast<VkInstance>(impl_->vkInstance)
            || volkGetLoadedDevice() != static_cast<VkDevice>(impl_->vkDevice))
            throw std::logic_error("VulkanContext: global Volk device dispatch was replaced by a foreign device; the shared renderer requires its original dispatch");
        return;
    }
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

    vk::PhysicalDeviceIDProperties idProps;
    vk::PhysicalDeviceProperties2 props2;
    props2.pNext = &idProps;
    impl_->vkPhysDevice.getProperties2(&props2);
    impl_->deviceLuidValidFlag = idProps.deviceLUIDValid;
    impl_->deviceLuid = idProps.deviceLUID;

    auto props = impl_->vkPhysDevice.getProperties();
    LOG_INFO("VulkanContext: selected physical device — {}", props.deviceName.data());

    auto qf = findGraphicsQueueFamily(impl_->vkPhysDevice);
    if (!qf) {
        throw std::runtime_error("No graphics queue family found");
    }
    impl_->graphicsQF = *qf;

    auto computeQFOpt = findAloneCompute(impl_->vkPhysDevice);
    impl_->aloneCompute = computeQFOpt.has_value();
    impl_->computeQF    = impl_->aloneCompute ? *computeQFOpt : impl_->graphicsQF;

    LOG_INFO("VulkanContext: graphicsQF={}, computeQF={}, aloneCompute={}",
             impl_->graphicsQF, impl_->computeQF, impl_->aloneCompute);

    auto availableExts = impl_->vkPhysDevice.enumerateDeviceExtensionProperties();
    LOG_INFO("VulkanContext: {} device extensions available", availableExts.size());

    auto requestedExts = requiredDeviceExtensions();
    std::vector<const char*> devExts;
    for (auto* ext : requestedExts) {
        bool found = false;
        for (auto& avail : availableExts) {
            if (std::strcmp(avail.extensionName.data(), ext) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (std::strcmp(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0
                || std::strcmp(ext, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) {
                throw std::runtime_error(std::string("VulkanContext: required swapchain extension unavailable: ") + ext);
            }
            LOG_WARN("VulkanContext: optional external-sharing extension '{}' "
                     "is unavailable; hardware interop will be disabled", ext);
            continue;
        }
        devExts.push_back(ext);
    }
    LOG_INFO("VulkanContext: enabling {} device extensions", devExts.size());

    float queuePriority = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueInfos;

    vk::DeviceQueueCreateInfo gfxQInfo;
    gfxQInfo.queueFamilyIndex = impl_->graphicsQF;
    gfxQInfo.queueCount       = 1;
    gfxQInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(gfxQInfo);

    if (impl_->aloneCompute) {
        vk::DeviceQueueCreateInfo compQInfo;
        compQInfo.queueFamilyIndex = impl_->computeQF;
        compQInfo.queueCount       = 1;
        compQInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(compQInfo);
    }

    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceSynchronization2Features supportedSync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    supported.pNext = &supported11;
    supported11.pNext = &supported12;
    supported12.pNext = &supportedSync2;
    vkGetPhysicalDeviceFeatures2(impl_->vkPhysDevice, &supported);
    if (!supported12.timelineSemaphore || !supported12.hostQueryReset
        || !supported.features.shaderStorageImageExtendedFormats) {
        throw std::runtime_error("Vulkan device lacks timelineSemaphore, hostQueryReset or extended storage image formats required by the renderer");
    }
    if (!supportedSync2.synchronization2)
        throw std::runtime_error("Vulkan device lacks synchronization2 required by the libplacebo swapchain");
    impl_->enabledFeatures.pNext = &impl_->enabled11;
    impl_->enabled11.pNext = &impl_->enabled12;
    impl_->enabled12.pNext = &impl_->enabledSync2;
    impl_->enabledSync2.synchronization2 = VK_TRUE;
    impl_->enabledFeatures.features.shaderStorageImageExtendedFormats = VK_TRUE;
    impl_->enabled11.shaderDrawParameters = supported11.shaderDrawParameters;
    impl_->enabled12.timelineSemaphore = VK_TRUE;
    impl_->enabled12.hostQueryReset = VK_TRUE;
    impl_->enabledExtensions = devExts;

    vk::DeviceCreateInfo deviceInfo;
    deviceInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos       = queueInfos.data();
    deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    deviceInfo.ppEnabledExtensionNames = devExts.data();
    deviceInfo.pNext                   = &impl_->enabledFeatures;

    impl_->vkDevice = impl_->vkPhysDevice.createDevice(deviceInfo);

    volkLoadDevice(impl_->vkDevice);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl_->vkInstance, impl_->vkDevice);

    impl_->graphicsQueue = impl_->vkDevice.getQueue(impl_->graphicsQF, 0);
    impl_->computeQueue  = impl_->vkDevice.getQueue(impl_->computeQF, 0);

    LOG_INFO("VulkanContext: VkDevice created — graphicsQF={}, computeQF={}",
             impl_->graphicsQF, impl_->computeQF);
}

PFN_vkGetInstanceProcAddr VulkanContext::getInstanceProcAddr() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->vkGetInstanceProcAddr;
}

vk::Instance VulkanContext::vkInstance() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->vkInstance;
}

vk::PhysicalDevice VulkanContext::physicalDevice() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->vkPhysDevice;
}

vk::Device VulkanContext::device() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->vkDevice;
}

uint32_t VulkanContext::graphicsQueueFamily() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->graphicsQF;
}

vk::Queue VulkanContext::graphicsQueue() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->graphicsQueue;
}

uint32_t VulkanContext::computeQueueFamily() const {
    return impl_->computeQF;
}

vk::Queue VulkanContext::computeQueue() const {
    return impl_->computeQueue;
}

bool VulkanContext::hasAloneCompute() const {
    return impl_->aloneCompute;
}

const std::array<uint8_t, VK_LUID_SIZE>& VulkanContext::deviceLuid() const {
    return impl_->deviceLuid;
}

bool VulkanContext::deviceLuidValid() const {
    return impl_->deviceLuidValidFlag;
}

uint64_t VulkanContext::validationErrorCount() const {
    return impl_->validationErrors.load(std::memory_order_relaxed);
}

const VkPhysicalDeviceFeatures2* VulkanContext::enabledDeviceFeatures() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->vkDevice ? &impl_->enabledFeatures : nullptr;
}

std::vector<const char*> VulkanContext::enabledDeviceExtensions() const {
    std::lock_guard lock(impl_->initializationMutex);
    return impl_->enabledExtensions;
}

} // namespace renderer
} // namespace heisenberg
