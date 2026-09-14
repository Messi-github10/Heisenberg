#include "D3D11VulkanInterop.hpp"

#include <Utiles/Logger.hpp>
#include <Video/Renderer/ColorSpaceUtils.hpp>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <volk.h>

extern "C" {
#include <libplacebo/d3d11.h>
#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
}
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace heisenberg::renderer {
namespace {

constexpr size_t kFramePoolSize = 6;

const char* plLogLevelName(enum pl_log_level level) {
    switch (level) {
    case PL_LOG_FATAL: return "fatal";
    case PL_LOG_ERR: return "error";
    case PL_LOG_WARN: return "warn";
    case PL_LOG_INFO: return "info";
    case PL_LOG_DEBUG: return "debug";
    case PL_LOG_TRACE: return "trace";
    default: return "none";
    }
}

void d3d11LibplaceboLog(void*, enum pl_log_level level, const char* message) {
    if (!message) return;
    // Keep the callback independent of libplacebo's stderr logger so every
    // message appears in the application's timestamped log.
    LOG_INFO("[libplacebo:d3d11:{}] {}", plLogLevelName(level), message);
}

void logTextureDesc(const char* name, ID3D11Texture2D* texture) {
    if (!texture) {
        LOG_ERROR("D3D11VulkanInterop: {} texture is null", name);
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    LOG_INFO("D3D11VulkanInterop: {} desc: {}x{} format={} mip={} array={} "
             "bind=0x{:x} misc=0x{:x} usage={} samples={}",
             name, desc.Width, desc.Height, static_cast<int>(desc.Format),
             desc.MipLevels, desc.ArraySize, desc.BindFlags, desc.MiscFlags,
             static_cast<int>(desc.Usage), desc.SampleDesc.Count);
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0
            && (properties.memoryTypes[i].propertyFlags & preferred)
                   == preferred) {
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0) return i;
    }
    return UINT32_MAX;
}

bool createTimelineSemaphore(VkDevice device, VkSemaphore* out,
                             VkExternalSemaphoreHandleTypeFlags exportTypes = 0) {
    VkSemaphoreTypeCreateInfo typeInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkExportSemaphoreCreateInfo exportInfo{
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = exportTypes;
    if (exportTypes != 0) typeInfo.pNext = &exportInfo;

    VkSemaphoreCreateInfo createInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &typeInfo;
    return vkCreateSemaphore(device, &createInfo, nullptr, out) == VK_SUCCESS;
}

void signalD3D11Fence(ID3D11DeviceContext* context, ID3D11Fence* fence,
                      uint64_t value) {
    ID3D11DeviceContext4* context4 = nullptr;
    if (context && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
        context4->Signal(fence, value);
        context4->Release();
    }
}

} // namespace

struct D3D11VulkanInterop::Impl {
    struct FrameResource {
        ID3D11Texture2D* sharedTexture = nullptr;
        HANDLE sharedHandle = nullptr;

        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;

        ID3D11Fence* d3dFence = nullptr;
        HANDLE fenceHandle = nullptr;
        VkSemaphore d3dDoneSemaphore = VK_NULL_HANDLE;
        uint64_t fenceValue = 0;

        VkSemaphore reuseSemaphore = VK_NULL_HANDLE;
        uint64_t reuseValue = 0;
    };

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;

    pl_log log = nullptr;
    pl_d3d11 d3d = nullptr;
    pl_gpu gpu = nullptr;
    pl_renderer renderer = nullptr;

    int width = 0;
    int height = 0;
    uint64_t diagnosticFrames = 0;
    std::vector<FrameResource> frames;
    size_t nextSlot = 0;
    int currentSlot = -1;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer acquireCommand = VK_NULL_HANDLE;
    VkCommandBuffer releaseCommand = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    VkFence releaseFence = VK_NULL_HANDLE;
    VkSemaphore readySemaphore = VK_NULL_HANDLE;
    uint64_t readyValue = 0;

    bool createSharedTexture(FrameResource& frame) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET
                       | D3D11_BIND_SHADER_RESOURCE
                       | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED
                       | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        const HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr,
                                                       &frame.sharedTexture);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11VulkanInterop: CreateTexture2D failed (0x{:08x})",
                      static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    bool createFence(FrameResource& frame) {
        const auto importSemaphore =
            reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
                vkGetDeviceProcAddr(vkDevice,
                                    "vkImportSemaphoreWin32HandleKHR"));
        if (!importSemaphore) return false;
        ID3D11Device5* device5 = nullptr;
        if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
            return false;
        }
        const HRESULT fenceResult = device5->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&frame.d3dFence));
        device5->Release();
        if (FAILED(fenceResult)) return false;

        if (FAILED(frame.d3dFence->CreateSharedHandle(
                nullptr, GENERIC_ALL, nullptr, &frame.fenceHandle))) {
            return false;
        }
        if (!createTimelineSemaphore(
                vkDevice, &frame.d3dDoneSemaphore,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)) {
            return false;
        }

        VkImportSemaphoreWin32HandleInfoKHR importInfo{
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        importInfo.semaphore = frame.d3dDoneSemaphore;
        importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        importInfo.handle = frame.fenceHandle;
        if (importSemaphore(vkDevice, &importInfo) != VK_SUCCESS) {
            return false;
        }
        // Successful import transfers ownership of the NT handle to Vulkan.
        frame.fenceHandle = nullptr;
        return createTimelineSemaphore(vkDevice, &frame.reuseSemaphore);
    }

    bool importVulkanImage(FrameResource& frame) {
        if (frame.image != VK_NULL_HANDLE) return true;

        LOG_INFO("D3D11VulkanInterop: importing shared texture (handle={})",
                 static_cast<const void*>(frame.sharedHandle));

        IDXGIResource1* resource = nullptr;
        if (FAILED(frame.sharedTexture->QueryInterface(IID_PPV_ARGS(&resource)))) {
            return false;
        }
        const HRESULT handleResult = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &frame.sharedHandle);
        resource->Release();
        if (FAILED(handleResult)) {
            LOG_ERROR("D3D11VulkanInterop: CreateSharedHandle failed (0x{:08x})",
                      static_cast<unsigned>(handleResult));
            return false;
        }

        DWORD handleFlags = 0;
        const BOOL handleInfoOk =
            GetHandleInformation(frame.sharedHandle, &handleFlags);
        LOG_INFO("D3D11VulkanInterop: shared NT handle created={} valid={} "
                 "flags=0x{:x}",
                 static_cast<const void*>(frame.sharedHandle),
                 handleInfoOk ? 1 : 0, handleFlags);

        // Ask the Vulkan driver which memory types are compatible with this
        // exact Win32 handle.  The image memory requirements alone are not
        // sufficient for imported allocations; using a type accepted for a
        // normal image can produce VK_ERROR_OUT_OF_DEVICE_MEMORY here.
        uint32_t handleMemoryTypeBits = 0;
        const auto getHandleProperties =
            reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
                vkGetDeviceProcAddr(vkDevice,
                                    "vkGetMemoryWin32HandlePropertiesKHR"));
        if (getHandleProperties) {
            VkMemoryWin32HandlePropertiesKHR handleProperties{
                VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
            const VkResult propertiesResult = getHandleProperties(
                vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                frame.sharedHandle, &handleProperties);
            LOG_INFO("D3D11VulkanInterop: D3D11 texture handle properties "
                     "result={} memoryTypeBits=0x{:x}",
                     static_cast<int>(propertiesResult),
                     handleProperties.memoryTypeBits);
            if (propertiesResult == VK_SUCCESS)
                handleMemoryTypeBits = handleProperties.memoryTypeBits;
        } else {
            LOG_WARN("D3D11VulkanInterop: vkGetMemoryWin32HandlePropertiesKHR "
                     "is unavailable");
        }

        VkExternalMemoryImageCreateInfo externalInfo{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = &externalInfo;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        const VkResult createImageResult =
            vkCreateImage(vkDevice, &imageInfo, nullptr, &frame.image);
        if (createImageResult != VK_SUCCESS) {
            LOG_ERROR("D3D11VulkanInterop: vkCreateImage failed ({})",
                      static_cast<int>(createImageResult));
            destroyVulkan(frame);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(vkDevice, frame.image, &requirements);
        const uint32_t compatibleTypeBits =
            handleMemoryTypeBits != 0
                ? (requirements.memoryTypeBits & handleMemoryTypeBits)
                : requirements.memoryTypeBits;
        LOG_INFO("D3D11VulkanInterop: Vulkan image memory requirements: size={} "
                 "alignment={} typeBits=0x{:x} handleTypeBits=0x{:x} "
                 "compatibleTypeBits=0x{:x}",
                 requirements.size, requirements.alignment,
                 requirements.memoryTypeBits, handleMemoryTypeBits,
                 compatibleTypeBits);
        if (compatibleTypeBits == 0) {
            LOG_ERROR("D3D11VulkanInterop: no Vulkan memory type is compatible "
                      "with both the image and imported D3D11 handle");
            destroyVulkan(frame);
            return false;
        }
        uint32_t memoryType = findMemoryType(
            vkPhysicalDevice, compatibleTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            memoryType = findMemoryType(vkPhysicalDevice,
                                        compatibleTypeBits, 0);
        }
        if (memoryType == UINT32_MAX) {
            LOG_ERROR("D3D11VulkanInterop: no compatible Vulkan memory type");
            destroyVulkan(frame);
            return false;
        }
        LOG_INFO("D3D11VulkanInterop: selected Vulkan memory type {}", memoryType);

        VkImportMemoryWin32HandleInfoKHR importInfo{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        importInfo.handle = frame.sharedHandle;

        // D3D11 texture handles are dedicated external allocations.  The
        // image must be attached to VkMemoryDedicatedAllocateInfo; omitting
        // it causes NVIDIA's driver to reject the import as out of memory.
        VkMemoryDedicatedAllocateInfo dedicatedInfo{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = frame.image;
        dedicatedInfo.pNext = &importInfo;

        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicatedInfo;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        const VkResult allocateResult =
            vkAllocateMemory(vkDevice, &allocation, nullptr, &frame.memory);
        if (allocateResult != VK_SUCCESS) {
            LOG_ERROR("D3D11VulkanInterop: vkAllocateMemory failed ({})",
                      static_cast<int>(allocateResult));
            destroyVulkan(frame);
            return false;
        }
        const VkResult bindResult =
            vkBindImageMemory(vkDevice, frame.image, frame.memory, 0);
        if (bindResult != VK_SUCCESS) {
            LOG_ERROR("D3D11VulkanInterop: vkBindImageMemory failed ({})",
                      static_cast<int>(bindResult));
            destroyVulkan(frame);
            return false;
        }
        // Successful import transfers ownership of the NT handle to Vulkan.
        frame.sharedHandle = nullptr;

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = frame.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        const VkResult viewResult =
            vkCreateImageView(vkDevice, &viewInfo, nullptr, &frame.view);
        if (viewResult != VK_SUCCESS) {
            LOG_ERROR("D3D11VulkanInterop: vkCreateImageView failed ({})",
                      static_cast<int>(viewResult));
            destroyVulkan(frame);
            return false;
        }
        LOG_INFO("D3D11VulkanInterop: Vulkan shared image import succeeded");
        return true;
    }

    void destroyVulkan(FrameResource& frame) {
        if (frame.view) vkDestroyImageView(vkDevice, frame.view, nullptr);
        if (frame.image) vkDestroyImage(vkDevice, frame.image, nullptr);
        if (frame.memory) vkFreeMemory(vkDevice, frame.memory, nullptr);
        frame.view = VK_NULL_HANDLE;
        frame.image = VK_NULL_HANDLE;
        frame.memory = VK_NULL_HANDLE;
        if (frame.sharedHandle) CloseHandle(frame.sharedHandle);
        frame.sharedHandle = nullptr;
    }

    void destroyFrame(FrameResource& frame) {
        destroyVulkan(frame);
        if (frame.reuseSemaphore) {
            vkDestroySemaphore(vkDevice, frame.reuseSemaphore, nullptr);
            frame.reuseSemaphore = VK_NULL_HANDLE;
        }
        if (frame.d3dDoneSemaphore) {
            vkDestroySemaphore(vkDevice, frame.d3dDoneSemaphore, nullptr);
            frame.d3dDoneSemaphore = VK_NULL_HANDLE;
        }
        if (frame.fenceHandle) CloseHandle(frame.fenceHandle);
        frame.fenceHandle = nullptr;
        if (frame.d3dFence) frame.d3dFence->Release();
        frame.d3dFence = nullptr;
        if (frame.sharedTexture) frame.sharedTexture->Release();
        frame.sharedTexture = nullptr;
    }
};

D3D11VulkanInterop::D3D11VulkanInterop()
    : impl_(std::make_unique<Impl>()) {}

D3D11VulkanInterop::~D3D11VulkanInterop() {
    shutdown();
}

bool D3D11VulkanInterop::init(ID3D11Device* device,
                              ID3D11DeviceContext* context,
                              VkDevice vkDevice,
                              VkPhysicalDevice vkPhysicalDevice,
                              VkQueue graphicsQueue,
                              uint32_t graphicsQueueFamily,
                              int width, int height) {
    if (!device || !context || !vkDevice || !vkPhysicalDevice ||
        !graphicsQueue || width <= 0 || height <= 0) return false;
    shutdown();
    impl_->d3dDevice = device;
    impl_->d3dContext = context;
    impl_->vkDevice = vkDevice;
    impl_->vkPhysicalDevice = vkPhysicalDevice;
    impl_->graphicsQueue = graphicsQueue;
    impl_->graphicsQueueFamily = graphicsQueueFamily;
    impl_->width = width;
    impl_->height = height;

    // External memory sharing is only defined when both APIs address the
    // same physical adapter. Refuse a mismatched device pair early.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc{};
    bool adapterLuidValid = false;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))
        && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))
        && SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        adapterLuidValid = true;
    }
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    VkPhysicalDeviceIDProperties idProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(vkPhysicalDevice, &properties2);
    if (adapterLuidValid && idProperties.deviceLUIDValid) {
        LUID vulkanLuid{};
        static_assert(sizeof(vulkanLuid) == VK_LUID_SIZE);
        std::memcpy(&vulkanLuid, idProperties.deviceLUID, sizeof(vulkanLuid));
        if (adapterDesc.AdapterLuid.LowPart != vulkanLuid.LowPart
            || adapterDesc.AdapterLuid.HighPart != vulkanLuid.HighPart) {
            LOG_ERROR("D3D11VulkanInterop: D3D11/Vulkan adapter LUID mismatch");
            shutdown();
            return false;
        }
    }

    struct pl_log_params logParams{};
    logParams.log_cb = d3d11LibplaceboLog;
    logParams.log_level = PL_LOG_ALL;
    impl_->log = pl_log_create(PL_API_VER, &logParams);
    if (!impl_->log) return false;

    struct pl_d3d11_params d3dParams = pl_d3d11_default_params;
    d3dParams.device = device;
    d3dParams.allow_software = false;
    impl_->d3d = pl_d3d11_create(impl_->log, &d3dParams);
    if (!impl_->d3d) {
        shutdown();
        return false;
    }
    impl_->gpu = impl_->d3d->gpu;
    impl_->renderer = pl_renderer_create(impl_->log, impl_->gpu);
    if (!impl_->renderer) {
        shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &impl_->commandPool)
        != VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = impl_->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 2;
    VkCommandBuffer commands[2]{};
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, commands)
        != VK_SUCCESS) {
        shutdown();
        return false;
    }
    impl_->acquireCommand = commands[0];
    impl_->releaseCommand = commands[1];
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &impl_->acquireFence)
            != VK_SUCCESS
        || vkCreateFence(vkDevice, &fenceInfo, nullptr, &impl_->releaseFence)
               != VK_SUCCESS
        || !createTimelineSemaphore(vkDevice, &impl_->readySemaphore)) {
        shutdown();
        return false;
    }

    impl_->frames.resize(kFramePoolSize);
    for (auto& frame : impl_->frames) {
        if (!impl_->createSharedTexture(frame) || !impl_->createFence(frame)) {
            shutdown();
            return false;
        }
    }
    LOG_INFO("D3D11VulkanInterop: initialized {}x{} (pool {})",
             width, height, kFramePoolSize);
    return true;
}

void D3D11VulkanInterop::shutdown() {
    if (!impl_) return;
    if (impl_->vkDevice) {
        if (impl_->graphicsQueue) vkQueueWaitIdle(impl_->graphicsQueue);
        vkDeviceWaitIdle(impl_->vkDevice);
    }
    for (auto& frame : impl_->frames) impl_->destroyFrame(frame);
    impl_->frames.clear();
    if (impl_->readySemaphore) {
        vkDestroySemaphore(impl_->vkDevice, impl_->readySemaphore, nullptr);
        impl_->readySemaphore = VK_NULL_HANDLE;
    }
    if (impl_->acquireFence) vkDestroyFence(impl_->vkDevice, impl_->acquireFence, nullptr);
    if (impl_->releaseFence) vkDestroyFence(impl_->vkDevice, impl_->releaseFence, nullptr);
    if (impl_->commandPool) vkDestroyCommandPool(impl_->vkDevice, impl_->commandPool, nullptr);
    impl_->acquireFence = VK_NULL_HANDLE;
    impl_->releaseFence = VK_NULL_HANDLE;
    impl_->commandPool = VK_NULL_HANDLE;
    impl_->acquireCommand = VK_NULL_HANDLE;
    impl_->releaseCommand = VK_NULL_HANDLE;
    if (impl_->renderer) pl_renderer_destroy(&impl_->renderer);
    if (impl_->d3d) pl_d3d11_destroy(&impl_->d3d);
    if (impl_->log) pl_log_destroy(&impl_->log);
    impl_->gpu = nullptr;
    impl_->d3dDevice = nullptr;
    impl_->d3dContext = nullptr;
    impl_->vkDevice = VK_NULL_HANDLE;
    impl_->vkPhysicalDevice = VK_NULL_HANDLE;
    impl_->graphicsQueue = VK_NULL_HANDLE;
    impl_->graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    impl_->width = 0;
    impl_->height = 0;
    impl_->nextSlot = 0;
    impl_->currentSlot = -1;
    impl_->readyValue = 0;
}

bool D3D11VulkanInterop::resize(int width, int height) {
    if (!impl_ || width <= 0 || height <= 0) return false;
    if (impl_->width == width && impl_->height == height) return true;
    ID3D11Device* device = impl_->d3dDevice;
    ID3D11DeviceContext* context = impl_->d3dContext;
    VkDevice vkDevice = impl_->vkDevice;
    VkPhysicalDevice physicalDevice = impl_->vkPhysicalDevice;
    VkQueue queue = impl_->graphicsQueue;
    uint32_t queueFamily = impl_->graphicsQueueFamily;
    shutdown();
    return init(device, context, vkDevice, physicalDevice, queue, queueFamily,
                width, height);
}

bool D3D11VulkanInterop::processFrame(const AVFrame* hwFrame,
                                      filtergraph::VulkanImageRef& out) {
    if (!impl_ || !hwFrame || hwFrame->format != AV_PIX_FMT_D3D11
        || !hwFrame->data[0] || impl_->frames.empty()) return false;

    auto* decoderTexture = reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]);
    const int arraySlice = static_cast<int>(reinterpret_cast<intptr_t>(hwFrame->data[1]));
    bool is10Bit = false;
    if (hwFrame->hw_frames_ctx) {
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(
            hwFrame->hw_frames_ctx->data);
        is10Bit = framesContext && framesContext->sw_format == AV_PIX_FMT_P010;
    }

    const uint64_t diagnosticFrame = ++impl_->diagnosticFrames;
    if (diagnosticFrame <= 3 || diagnosticFrame % 60 == 0) {
        LOG_INFO("D3D11VulkanInterop: frame#{} AVFrame: format={} size={}x{} "
                 "data0={} data1={} arraySlice={} colorPrimaries={} "
                 "colorTransfer={} colorspace={} range={} chroma={}",
                 diagnosticFrame, hwFrame->format, hwFrame->width, hwFrame->height,
                 static_cast<const void*>(hwFrame->data[0]),
                 static_cast<const void*>(hwFrame->data[1]), arraySlice,
                 static_cast<int>(hwFrame->color_primaries),
                 static_cast<int>(hwFrame->color_trc),
                 static_cast<int>(hwFrame->colorspace),
                 static_cast<int>(hwFrame->color_range),
                 static_cast<int>(hwFrame->chroma_location));
        logTextureDesc("decoder", decoderTexture);
        logTextureDesc("shared-target", impl_->frames[impl_->nextSlot].sharedTexture);
    }

    Impl::FrameResource& frame = impl_->frames[impl_->nextSlot];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->frames.size();
    impl_->currentSlot = static_cast<int>(&frame - impl_->frames.data());
    if (frame.reuseValue != 0) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &frame.reuseSemaphore;
        waitInfo.pValues = &frame.reuseValue;
        if (vkWaitSemaphores(impl_->vkDevice, &waitInfo, UINT64_MAX)
            != VK_SUCCESS) return false;
    }

    const int uvWidth = (impl_->width + 1) / 2;
    const int uvHeight = (impl_->height + 1) / 2;
    pl_d3d11_wrap_params yParams{};
    yParams.tex = decoderTexture;
    yParams.array_slice = arraySlice;
    yParams.fmt = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    yParams.w = impl_->width;
    yParams.h = impl_->height;
    pl_tex yTexture = pl_d3d11_wrap(impl_->gpu, &yParams);
    if (!yTexture) {
        LOG_ERROR("D3D11VulkanInterop: failed to wrap decoder Y plane");
        return false;
    }

    pl_d3d11_wrap_params uvParams{};
    uvParams.tex = decoderTexture;
    uvParams.array_slice = arraySlice;
    uvParams.fmt = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    uvParams.w = uvWidth;
    uvParams.h = uvHeight;
    pl_tex uvTexture = pl_d3d11_wrap(impl_->gpu, &uvParams);
    if (!uvTexture) {
        LOG_ERROR("D3D11VulkanInterop: failed to wrap decoder UV plane");
        pl_tex_destroy(impl_->gpu, &yTexture);
        return false;
    }

    pl_frame source{};
    source.color = colorSpaceFromAvFrame(hwFrame);
    source.crop = {0, 0, static_cast<float>(impl_->width),
                   static_cast<float>(impl_->height)};
    source.repr.sys = colorSystemFromAvFrame(hwFrame);
    source.repr.levels = hwFrame->color_range == AVCOL_RANGE_JPEG
        ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    source.repr.alpha = PL_ALPHA_NONE;
    source.num_planes = 2;
    source.planes[0].texture = yTexture;
    source.planes[0].components = 1;
    source.planes[0].component_mapping[0] = 0;
    source.planes[1].texture = uvTexture;
    source.planes[1].components = 2;
    source.planes[1].component_mapping[0] = 1;
    source.planes[1].component_mapping[1] = 2;
    pl_frame_set_chroma_location(&source, PL_CHROMA_LEFT);
    if (is10Bit) {
        source.repr.bits.sample_depth = 16;
        source.repr.bits.color_depth = 10;
        source.repr.bits.bit_shift = 6;
    } else {
    source.repr.bits.sample_depth = 8;
        source.repr.bits.color_depth = 8;
        source.repr.bits.bit_shift = 0;
    }

    pl_d3d11_wrap_params dstParams{};
    dstParams.tex = frame.sharedTexture;
    pl_tex destination = pl_d3d11_wrap(impl_->gpu, &dstParams);
    if (!destination) {
        LOG_ERROR("D3D11VulkanInterop: failed to wrap shared RGBA destination");
        pl_tex_destroy(impl_->gpu, &yTexture);
        pl_tex_destroy(impl_->gpu, &uvTexture);
        return false;
    }

    pl_frame target{};
    target.num_planes = 1;
    target.planes[0].texture = destination;
    // The conversion target is RGB; alpha is not part of the decoded video
    // signal.  Keeping this as a 3-component frame matches libplacebo's D3D11
    // interop path and the reference implementation.
    target.planes[0].components = 3;
    target.planes[0].component_mapping[0] = 0;
    target.planes[0].component_mapping[1] = 1;
    target.planes[0].component_mapping[2] = 2;
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.repr.levels = PL_COLOR_LEVELS_FULL;
    // Keep the imported image in the same scene-linear working space used by
    // the software upload path. Display conversion is deferred until the
    // final swapchain render.
    target.color = workingColorSpace();
    target.crop = {0, 0, static_cast<float>(impl_->width),
                   static_cast<float>(impl_->height)};

    if (diagnosticFrame <= 3 || diagnosticFrame % 60 == 0) {
        LOG_INFO("D3D11VulkanInterop: frame#{} source: planes={} repr.sys={} "
                 "levels={} alpha={} bits={}/{}/{} color=({}, {}) crop={}x{}; "
                 "target: planes={} components={} repr.sys={} levels={} "
                 "alpha={} color=({}, {})",
                 diagnosticFrame, source.num_planes,
                 static_cast<int>(source.repr.sys),
                 static_cast<int>(source.repr.levels),
                 static_cast<int>(source.repr.alpha),
                 source.repr.bits.sample_depth, source.repr.bits.color_depth,
                 source.repr.bits.bit_shift,
                 static_cast<int>(source.color.primaries),
                 static_cast<int>(source.color.transfer), source.crop.x1 - source.crop.x0,
                 source.crop.y1 - source.crop.y0, target.num_planes,
                 target.planes[0].components,
                 static_cast<int>(target.repr.sys),
                 static_cast<int>(target.repr.levels),
                 static_cast<int>(target.repr.alpha),
                 static_cast<int>(target.color.primaries),
                 static_cast<int>(target.color.transfer));
    }

    const bool rendered = pl_render_image(impl_->renderer, &source, &target,
                                           &pl_render_default_params);
    pl_tex_destroy(impl_->gpu, &yTexture);
    pl_tex_destroy(impl_->gpu, &uvTexture);
    pl_tex_destroy(impl_->gpu, &destination);
    if (!rendered) {
        LOG_ERROR("D3D11VulkanInterop: libplacebo D3D11 NV12->RGBA render failed");
        return false;
    }

    impl_->d3dContext->Flush();
    ++frame.fenceValue;
    signalD3D11Fence(impl_->d3dContext, frame.d3dFence, frame.fenceValue);
    if (!impl_->importVulkanImage(frame)) {
        LOG_ERROR("D3D11VulkanInterop: failed to import shared RGBA texture into Vulkan");
        return false;
    }

    if (vkWaitForFences(impl_->vkDevice, 1, &impl_->acquireFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) return false;
    vkResetFences(impl_->vkDevice, 1, &impl_->acquireFence);
    vkResetCommandBuffer(impl_->acquireCommand, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(impl_->acquireCommand, &beginInfo) != VK_SUCCESS)
        return false;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = frame.reuseValue ? VK_IMAGE_LAYOUT_GENERAL
                                         : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = impl_->graphicsQueueFamily;
    barrier.image = frame.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(impl_->acquireCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(impl_->acquireCommand) != VK_SUCCESS) return false;

    const uint64_t fenceValue = frame.fenceValue;
    const uint64_t readyValue = ++impl_->readyValue;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = 1;
    timelineInfo.pWaitSemaphoreValues = &fenceValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &readyValue;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.d3dDoneSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &impl_->acquireCommand;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &impl_->readySemaphore;
    if (vkQueueSubmit(impl_->graphicsQueue, 1, &submitInfo,
                      impl_->acquireFence) != VK_SUCCESS) return false;

    out = {};
    out.image = frame.image;
    out.view = frame.view;
    out.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    out.extent = {static_cast<uint32_t>(impl_->width),
                  static_cast<uint32_t>(impl_->height)};
    out.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.queueFamilyIndex = impl_->graphicsQueueFamily;
    out.ready = {impl_->readySemaphore, readyValue};
    out.contract = filtergraph::kWorkingImageContract;
    return true;
}

void D3D11VulkanInterop::releaseFrame(
    const filtergraph::VulkanSyncPoint& done) {
    if (!impl_ || impl_->currentSlot < 0 || !done.valid()) return;
    Impl::FrameResource& frame = impl_->frames[static_cast<size_t>(impl_->currentSlot)];
    if (vkWaitForFences(impl_->vkDevice, 1, &impl_->releaseFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) return;
    vkResetFences(impl_->vkDevice, 1, &impl_->releaseFence);
    vkResetCommandBuffer(impl_->releaseCommand, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(impl_->releaseCommand, &beginInfo) != VK_SUCCESS)
        return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = impl_->graphicsQueueFamily;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = frame.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(impl_->releaseCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(impl_->releaseCommand) != VK_SUCCESS) return;

    const uint64_t doneValue = done.value;
    const uint64_t reuseValue = ++frame.reuseValue;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = 1;
    timelineInfo.pWaitSemaphoreValues = &doneValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &reuseValue;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &done.semaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &impl_->releaseCommand;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.reuseSemaphore;
    if (vkQueueSubmit(impl_->graphicsQueue, 1, &submitInfo,
                      impl_->releaseFence) == VK_SUCCESS) {
        impl_->currentSlot = -1;
    }
}

} // namespace heisenberg::renderer
