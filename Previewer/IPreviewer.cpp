#include "IPreviewer.hpp"

#include "Renderer/RenderEngine.hpp"
#include "Renderer/SwapChain.hpp"
#include "TextureContext/TextureManager.hpp"

#include <Common/FrameTime.hpp>
#include <FilterGraph/Interface/INodeFactory.hpp>
#include <FilterGraph/Interface/IPipeGraph.hpp>
#include <Utiles/Logger.hpp>

#include <vulkan/vulkan.hpp>

#include <exception>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libplacebo/renderer.h>
}

namespace heisenberg::renderer {
namespace {

VkImage toC(vk::Image image) {
    return static_cast<VkImage>(image);
}

VkImageView toC(vk::ImageView view) {
    return static_cast<VkImageView>(view);
}

VkSemaphore toC(vk::Semaphore semaphore) {
    return static_cast<VkSemaphore>(semaphore);
}

VkFormat toC(vk::Format format) {
    return static_cast<VkFormat>(format);
}

VkImageLayout toC(vk::ImageLayout layout) {
    return static_cast<VkImageLayout>(layout);
}

VkImageUsageFlags toCUsage(vk::ImageUsageFlags usage) {
    return static_cast<VkImageUsageFlags>(usage);
}

pl_frame makeRgbFrame(pl_tex texture, int width, int height,
                      const pl_color_space& color) {
    pl_frame frame = {};
    frame.num_planes = 1;
    frame.planes[0].texture = texture;
    frame.planes[0].components = 4;
    frame.planes[0].component_mapping[0] = 0;
    frame.planes[0].component_mapping[1] = 1;
    frame.planes[0].component_mapping[2] = 2;
    frame.planes[0].component_mapping[3] = 3;
    frame.repr.sys = PL_COLOR_SYSTEM_RGB;
    frame.repr.levels = PL_COLOR_LEVELS_PC;
    frame.repr.alpha = PL_ALPHA_INDEPENDENT;
    frame.color = color;
    frame.crop = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    return frame;
}

pl_color_space makeDisplayColor() {
    pl_color_space color = {};
    color.primaries = PL_COLOR_PRIM_BT_709;
    color.transfer = PL_COLOR_TRC_SRGB;
    return color;
}

pl_color_space makeWorkingColor(const pl_frame& source) {
    pl_color_space color = source.color;
    pl_color_space_infer(&color);
    color.primaries = PL_COLOR_PRIM_BT_2020;
    color.transfer = PL_COLOR_TRC_LINEAR;
    return color;
}

} // namespace

struct IPreviewer::Impl {
    pl_gpu gpu = nullptr;
    pl_vulkan vulkan = nullptr;

    vk::Device vkDevice;
    vk::PhysicalDevice vkPhysDevice;

    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<RenderEngine> renderEngine;
    std::unique_ptr<SwapChain> swapChain;

    ResizeCallback onResize;
    PresentCallback onPresent;

    int outputWidth = 0;
    int outputHeight = 0;
    bool initialized = false;

    vk::Image intermediateImg = nullptr;
    vk::ImageView intermediateView = nullptr;
    vk::DeviceMemory intermediateMemory = nullptr;
    vk::ImageLayout intermediateLayout = vk::ImageLayout::eUndefined;
    int intermediateWidth = 0;
    int intermediateHeight = 0;

    vk::Semaphore interopSemaphore;
    uint64_t interopValue = 0;
    filtergraph::VulkanSyncPoint intermediateReleaseWait = {};

    filtergraph::IPipeGraph* filterGraph = nullptr;
    filtergraph::IInputNode* dagInput = nullptr;
    filtergraph::IOutputNode* dagOutput = nullptr;
    pl_color_space workingColor = {};
};

IPreviewer::IPreviewer() : impl_(std::make_unique<Impl>()) {}

IPreviewer::~IPreviewer() {
    shutdown();
}

bool IPreviewer::initialize(pl_gpu gpu, pl_vulkan vulkan,
                            std::unique_ptr<SwapChain> swapChain,
                            int width, int height) {
    if (!gpu || !vulkan || !swapChain || !swapChain->isValid()) {
        LOG_ERROR("IPreviewer: invalid initialization parameters");
        return false;
    }

    impl_->gpu = gpu;
    impl_->vulkan = vulkan;
    impl_->vkDevice = vk::Device(vulkan->device);
    impl_->vkPhysDevice = vk::PhysicalDevice(vulkan->phys_device);
    impl_->swapChain = std::move(swapChain);
    impl_->outputWidth = width;
    impl_->outputHeight = height;

    vk::SemaphoreTypeCreateInfo timelineInfo;
    timelineInfo.semaphoreType = vk::SemaphoreType::eTimeline;
    timelineInfo.initialValue = 0;
    vk::SemaphoreCreateInfo semaphoreInfo;
    semaphoreInfo.pNext = &timelineInfo;
    impl_->interopSemaphore = impl_->vkDevice.createSemaphore(semaphoreInfo);

    try {
        impl_->textureManager = std::make_unique<TextureManager>(gpu);
        impl_->renderEngine = std::make_unique<RenderEngine>(gpu);
    } catch (const std::exception& error) {
        LOG_ERROR("IPreviewer: renderer initialization failed: {}", error.what());
        shutdown();
        return false;
    }

    impl_->initialized = true;
    LOG_INFO("IPreviewer: initialized {}x{}", width, height);
    return true;
}

void IPreviewer::shutdown() {
    if (!impl_) return;

    releaseIntermediateTarget();
    if (impl_->interopSemaphore) {
        impl_->vkDevice.destroySemaphore(impl_->interopSemaphore);
        impl_->interopSemaphore = nullptr;
    }
    if (impl_->swapChain) impl_->swapChain->shutdown();
    if (impl_->textureManager) impl_->textureManager->shutdown();
    impl_->renderEngine.reset();
    impl_->initialized = false;
}

void IPreviewer::resize(int width, int height) {
    impl_->outputWidth = width;
    impl_->outputHeight = height;
    if (impl_->swapChain) {
        int actualWidth = width;
        int actualHeight = height;
        impl_->swapChain->resize(&actualWidth, &actualHeight);
    }
    if (impl_->onResize) impl_->onResize(width, height);
}

void IPreviewer::setOnResize(ResizeCallback callback) {
    impl_->onResize = std::move(callback);
}

void IPreviewer::setOnPresent(PresentCallback callback) {
    impl_->onPresent = std::move(callback);
}

void IPreviewer::setFilterGraph(filtergraph::IPipeGraph* graph,
                                filtergraph::IInputNode* input,
                                filtergraph::IOutputNode* output) {
    impl_->filterGraph = graph;
    impl_->dagInput = input;
    impl_->dagOutput = output;
    impl_->intermediateReleaseWait = {};
}

bool IPreviewer::buildIntermediateTarget(int width, int height) {
    if (width <= 0 || height <= 0) return false;

    vk::ImageCreateInfo imageInfo;
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = vk::Format::eR16G16B16A16Sfloat;
    imageInfo.extent = vk::Extent3D{
        static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eStorage
        | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eColorAttachment
        | vk::ImageUsageFlagBits::eTransferSrc
        | vk::ImageUsageFlagBits::eTransferDst;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    impl_->intermediateImg = impl_->vkDevice.createImage(imageInfo);
    if (!impl_->intermediateImg) return false;

    const vk::MemoryRequirements requirements =
        impl_->vkDevice.getImageMemoryRequirements(impl_->intermediateImg);
    const vk::PhysicalDeviceMemoryProperties properties =
        impl_->vkPhysDevice.getMemoryProperties();

    uint32_t memoryType = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i))
            && (properties.memoryTypes[i].propertyFlags
                & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == properties.memoryTypeCount) {
        releaseIntermediateTarget();
        return false;
    }

    vk::MemoryAllocateInfo allocation;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    impl_->intermediateMemory = impl_->vkDevice.allocateMemory(allocation);
    if (!impl_->intermediateMemory) {
        releaseIntermediateTarget();
        return false;
    }
    impl_->vkDevice.bindImageMemory(
        impl_->intermediateImg, impl_->intermediateMemory, 0);

    vk::ImageViewCreateInfo viewInfo;
    viewInfo.image = impl_->intermediateImg;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = vk::Format::eR16G16B16A16Sfloat;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    impl_->intermediateView = impl_->vkDevice.createImageView(viewInfo);
    if (!impl_->intermediateView) {
        releaseIntermediateTarget();
        return false;
    }

    impl_->intermediateLayout = vk::ImageLayout::eUndefined;
    impl_->intermediateWidth = width;
    impl_->intermediateHeight = height;
    return true;
}

void IPreviewer::releaseIntermediateTarget() {
    if (!impl_->vkDevice) return;
    if (impl_->intermediateView || impl_->intermediateImg) {
        impl_->vkDevice.waitIdle();
    }
    if (impl_->intermediateView) {
        impl_->vkDevice.destroyImageView(impl_->intermediateView);
        impl_->intermediateView = nullptr;
    }
    if (impl_->intermediateImg) {
        impl_->vkDevice.destroyImage(impl_->intermediateImg);
        impl_->intermediateImg = nullptr;
    }
    if (impl_->intermediateMemory) {
        impl_->vkDevice.freeMemory(impl_->intermediateMemory);
        impl_->intermediateMemory = nullptr;
    }
    impl_->intermediateLayout = vk::ImageLayout::eUndefined;
    impl_->intermediateWidth = 0;
    impl_->intermediateHeight = 0;
    impl_->intermediateReleaseWait = {};
}

bool IPreviewer::presentFrame(const AVFrame* avframe) {
    if (!avframe || !impl_->initialized || !impl_->swapChain->isValid()) {
        return false;
    }
    const int outputWidth = impl_->outputWidth;
    const int outputHeight = impl_->outputHeight;
    if (outputWidth <= 0 || outputHeight <= 0) return false;

    const pl_frame* source = impl_->textureManager->uploadAvFrame(avframe);
    if (!source) return false;

    if (!impl_->filterGraph || !impl_->dagInput || !impl_->dagOutput) {
        const bool result =
            renderToSwapChain(source, outputWidth, outputHeight);
        if (result && impl_->onPresent) impl_->onPresent();
        return result;
    }

    const int workWidth = avframe->width;
    const int workHeight = avframe->height;
    if (workWidth <= 0 || workHeight <= 0) return false;
    if (!impl_->intermediateImg || impl_->intermediateWidth != workWidth
        || impl_->intermediateHeight != workHeight) {
        releaseIntermediateTarget();
        if (!buildIntermediateTarget(workWidth, workHeight)) return false;
    }

    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage
        | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eColorAttachment
        | vk::ImageUsageFlagBits::eTransferSrc
        | vk::ImageUsageFlagBits::eTransferDst;
    pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image = toC(impl_->intermediateImg);
    wrapParams.width = workWidth;
    wrapParams.height = workHeight;
    wrapParams.format = toC(vk::Format::eR16G16B16A16Sfloat);
    wrapParams.usage = toCUsage(usage);

    pl_tex targetTexture = pl_vulkan_wrap(impl_->gpu, &wrapParams);
    if (!targetTexture) return false;

    pl_vulkan_release_params releaseParams = {};
    releaseParams.tex = targetTexture;
    releaseParams.layout = toC(impl_->intermediateLayout);
    releaseParams.qf = impl_->vulkan->queue_graphics.index;
    releaseParams.semaphore.sem = impl_->intermediateReleaseWait.semaphore;
    releaseParams.semaphore.value = impl_->intermediateReleaseWait.value;
    pl_vulkan_release_ex(impl_->gpu, &releaseParams);

    impl_->workingColor = makeWorkingColor(*source);
    pl_frame workingFrame = makeRgbFrame(
        targetTexture, workWidth, workHeight, impl_->workingColor);
    if (!impl_->renderEngine->render(source, &workingFrame)) {
        pl_tex_destroy(impl_->gpu, &targetTexture);
        return false;
    }

    pl_gpu_flush(impl_->gpu);
    pl_vulkan_hold_params holdParams = {};
    holdParams.tex = targetTexture;
    holdParams.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    holdParams.qf = impl_->vulkan->queue_graphics.index;
    holdParams.semaphore.sem = toC(impl_->interopSemaphore);
    holdParams.semaphore.value = ++impl_->interopValue;
    if (!pl_vulkan_hold_ex(impl_->gpu, &holdParams)) {
        pl_tex_destroy(impl_->gpu, &targetTexture);
        return false;
    }
    pl_tex_destroy(impl_->gpu, &targetTexture);
    impl_->intermediateLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    filtergraph::VulkanImageRef graphInput;
    graphInput.image = toC(impl_->intermediateImg);
    graphInput.view = toC(impl_->intermediateView);
    graphInput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    graphInput.extent = {
        static_cast<uint32_t>(workWidth), static_cast<uint32_t>(workHeight)};
    graphInput.usage = toCUsage(usage);
    graphInput.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    graphInput.queueFamilyIndex = impl_->vulkan->queue_graphics.index;
    graphInput.ready = {toC(impl_->interopSemaphore), impl_->interopValue};
    graphInput.contract = filtergraph::kWorkingImageContract;
    if (!impl_->dagInput->setVulkanInput(graphInput)) return false;

    filtergraph::FrameContext frameContext;
    frameContext.pts = avframe->pts;
    frameContext.timeBaseNum = avframe->time_base.num;
    frameContext.timeBaseDen = avframe->time_base.den;
    frameContext.timeSeconds = frameTimeSeconds(*avframe);
    if (!impl_->filterGraph->run(frameContext)) return false;

    filtergraph::VulkanImageRef graphOutput;
    if (!impl_->dagOutput->getVulkanOutput(graphOutput)
        || !graphOutput.valid()) {
        impl_->vkDevice.waitIdle();
        return false;
    }
    impl_->intermediateReleaseWait = graphOutput.ready;

    const bool result = renderToSwapChain(graphOutput);
    if (result && impl_->onPresent) impl_->onPresent();
    return result;
}

bool IPreviewer::renderToSwapChain(
    const pl_frame* source, int width, int height) {
    if (!source) return false;
    pl_tex framebuffer = impl_->swapChain->startFrame(width, height);
    if (!framebuffer) return false;

    const pl_color_space displayColor = makeDisplayColor();
    pl_frame target = makeRgbFrame(framebuffer, width, height, displayColor);
    if (!impl_->renderEngine->render(source, &target)) return false;
    if (!impl_->swapChain->submitFrame()) return false;
    impl_->swapChain->swapBuffers();
    return true;
}

bool IPreviewer::renderToSwapChain(
    const filtergraph::VulkanImageRef& image) {
    if (!image.valid()) return false;

    pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image = image.image;
    wrapParams.width = static_cast<int>(image.extent.width);
    wrapParams.height = static_cast<int>(image.extent.height);
    wrapParams.format = image.format;
    wrapParams.usage = image.usage;
    pl_tex sourceTexture = pl_vulkan_wrap(impl_->gpu, &wrapParams);
    if (!sourceTexture) return false;

    pl_vulkan_release_params releaseParams = {};
    releaseParams.tex = sourceTexture;
    releaseParams.layout = image.layout;
    releaseParams.qf = image.queueFamilyIndex;
    releaseParams.semaphore.sem = image.ready.semaphore;
    releaseParams.semaphore.value = image.ready.value;
    pl_vulkan_release_ex(impl_->gpu, &releaseParams);

    pl_tex framebuffer = impl_->swapChain->startFrame(
        impl_->outputWidth, impl_->outputHeight);
    bool rendered = framebuffer != nullptr;
    if (rendered) {
        if (image.contract != filtergraph::kWorkingImageContract) {
            LOG_ERROR("IPreviewer: graph output violates the working image contract");
            rendered = false;
        }
        pl_frame source = makeRgbFrame(
            sourceTexture, static_cast<int>(image.extent.width),
            static_cast<int>(image.extent.height), impl_->workingColor);
        const pl_color_space displayColor = makeDisplayColor();
        pl_frame target = makeRgbFrame(framebuffer, impl_->outputWidth,
                                       impl_->outputHeight, displayColor);
        if (rendered) rendered = impl_->renderEngine->render(&source, &target);
    }

    pl_gpu_flush(impl_->gpu);
    pl_vulkan_hold_params holdParams = {};
    holdParams.tex = sourceTexture;
    holdParams.layout = image.layout;
    holdParams.qf = image.queueFamilyIndex;
    holdParams.semaphore.sem = toC(impl_->interopSemaphore);
    holdParams.semaphore.value = ++impl_->interopValue;
    if (!pl_vulkan_hold_ex(impl_->gpu, &holdParams)) {
        pl_tex_destroy(impl_->gpu, &sourceTexture);
        return false;
    }
    pl_tex_destroy(impl_->gpu, &sourceTexture);

    filtergraph::VulkanImageRef returnedImage = image;
    returnedImage.ready = {
        toC(impl_->interopSemaphore), impl_->interopValue};
    impl_->dagOutput->releaseVulkanOutput(returnedImage);
    if (image.image == toC(impl_->intermediateImg)) {
        impl_->intermediateReleaseWait = returnedImage.ready;
    }

    if (!rendered || !impl_->swapChain->submitFrame()) return false;
    impl_->swapChain->swapBuffers();
    return true;
}

} // namespace heisenberg::renderer
