//
// Created by NiceFold on 2026/7/9.
//

#include "IPreviewer.hpp"
#include "Renderer/RenderEngine.hpp"
#include "Renderer/SwapChain.hpp"
#include "TextureContext/TextureManager.hpp"

#include <FilterGraph/Interface/ILayerFactory.hpp>
#include <FilterGraph/Interface/IPipeGraph.hpp>
#include <Utiles/Logger.hpp>

#include <vulkan/vulkan.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libplacebo/renderer.h>
}

namespace heisenberg {
namespace renderer {

// ============================================================
// Impl
// ============================================================

struct IPreviewer::Impl {
    pl_gpu    gpu    = nullptr;
    pl_vulkan vulkan = nullptr;

    vk::Device         vkDevice;
    vk::PhysicalDevice vkPhysDevice;

    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<RenderEngine>   renderEngine;
    std::unique_ptr<SwapChain>      swapChain;

    ResizeCallback  onResize;
    PresentCallback onPresent;

    int outputWidth  = 0;
    int outputHeight = 0;
    bool initialized = false;

    vk::Image        intermediateImg    = nullptr;
    vk::DeviceMemory intermediateMemory = nullptr;
    vk::ImageLayout  intermediateLayout = vk::ImageLayout::eUndefined;

    vk::Semaphore holdSemaphore;
    vk::Semaphore releaseSemaphore;

    heisenberg::filtergraph::IPipeGraph*  filterGraph = nullptr;
    heisenberg::filtergraph::IInputLayer* dagInput    = nullptr;
    heisenberg::filtergraph::IOutputLayer* dagOutput  = nullptr;
};

IPreviewer::IPreviewer()
    : impl_(std::make_unique<Impl>()) {}

IPreviewer::~IPreviewer() {
    shutdown();
}

bool IPreviewer::initialize(pl_gpu gpu, pl_vulkan vk,
                            std::unique_ptr<SwapChain> swapChain,
                            int width, int height) {
    if (!gpu || !vk || !swapChain || !swapChain->isValid()) {
        LOG_ERROR("IPreviewer: invalid parameters");
        return false;
    }

    impl_->gpu    = gpu;
    impl_->vulkan = vk;
    impl_->vkDevice     = vk::Device(vk->device);
    impl_->vkPhysDevice = vk::PhysicalDevice(vk->phys_device);
    impl_->swapChain = std::move(swapChain);
    impl_->outputWidth  = width;
    impl_->outputHeight = height;

    vk::SemaphoreCreateInfo semInfo;
    impl_->holdSemaphore    = impl_->vkDevice.createSemaphore(semInfo);
    impl_->releaseSemaphore = impl_->vkDevice.createSemaphore(semInfo);

    try {
        impl_->textureManager = std::make_unique<TextureManager>(gpu);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: TextureManager failed — {}", e.what());
        return false;
    }

    try {
        impl_->renderEngine = std::make_unique<RenderEngine>(gpu);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: RenderEngine failed — {}", e.what());
        impl_->textureManager.reset();
        return false;
    }

    if (!buildIntermediateTarget(width, height)) {
        impl_->textureManager.reset();
        impl_->renderEngine.reset();
        return false;
    }

    impl_->initialized = true;
    LOG_INFO("IPreviewer: initialized — {}x{}", width, height);
    return true;
}

void IPreviewer::shutdown() {
    releaseIntermediateTarget();

    if (impl_->holdSemaphore) {
        impl_->vkDevice.destroySemaphore(impl_->holdSemaphore);
        impl_->holdSemaphore = nullptr;
    }
    if (impl_->releaseSemaphore) {
        impl_->vkDevice.destroySemaphore(impl_->releaseSemaphore);
        impl_->releaseSemaphore = nullptr;
    }

    if (impl_->swapChain) {
        impl_->swapChain->shutdown();
    }
    if (impl_->textureManager) {
        impl_->textureManager->shutdown();
    }
    impl_->renderEngine.reset();
    impl_->initialized = false;
}

void IPreviewer::resize(int width, int height) {
    impl_->outputWidth  = width;
    impl_->outputHeight = height;
    if (impl_->swapChain) {
        int w = width, h = height;
        impl_->swapChain->resize(&w, &h);
    }
    releaseIntermediateTarget();
    buildIntermediateTarget(width, height);
    if (impl_->onResize) {
        impl_->onResize(width, height);
    }
}

void IPreviewer::setOnResize(ResizeCallback cb) {
    impl_->onResize = std::move(cb);
}

void IPreviewer::setOnPresent(PresentCallback cb) {
    impl_->onPresent = std::move(cb);
}

void IPreviewer::setFilterGraph(
    heisenberg::filtergraph::IPipeGraph* graph,
    heisenberg::filtergraph::IInputLayer* input,
    heisenberg::filtergraph::IOutputLayer* output) {
    impl_->filterGraph = graph;
    impl_->dagInput    = input;
    impl_->dagOutput   = output;
}

bool IPreviewer::buildIntermediateTarget(int width, int height) {
    if (width <= 0 || height <= 0) return false;

    vk::ImageCreateInfo imgInfo;
    imgInfo.imageType     = vk::ImageType::e2D;
    imgInfo.format        = vk::Format::eR8G8B8A8Unorm;
    imgInfo.extent        = vk::Extent3D{ static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = vk::SampleCountFlagBits::e1;
    imgInfo.tiling        = vk::ImageTiling::eOptimal;
    imgInfo.usage         = vk::ImageUsageFlagBits::eStorage
                          | vk::ImageUsageFlagBits::eSampled
                          | vk::ImageUsageFlagBits::eColorAttachment
                          | vk::ImageUsageFlagBits::eTransferSrc
                          | vk::ImageUsageFlagBits::eTransferDst;
    imgInfo.sharingMode   = vk::SharingMode::eExclusive;
    imgInfo.initialLayout = vk::ImageLayout::eUndefined;

    impl_->intermediateImg = impl_->vkDevice.createImage(imgInfo);
    if (!impl_->intermediateImg) {
        LOG_ERROR("IPreviewer: vkCreateImage() for intermediate target failed");
        return false;
    }

    vk::MemoryRequirements memReq = impl_->vkDevice.getImageMemoryRequirements(impl_->intermediateImg);

    vk::MemoryAllocateInfo allocInfo;
    allocInfo.allocationSize = memReq.size;

    vk::PhysicalDeviceMemoryProperties memProps = impl_->vkPhysDevice.getMemoryProperties();
    bool found = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i))
            && (memProps.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            allocInfo.memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR("IPreviewer: no suitable device-local memory type");
        impl_->vkDevice.destroyImage(impl_->intermediateImg);
        impl_->intermediateImg = nullptr;
        return false;
    }

    impl_->intermediateMemory = impl_->vkDevice.allocateMemory(allocInfo);
    if (!impl_->intermediateMemory) {
        LOG_ERROR("IPreviewer: vkAllocateMemory() for intermediate target failed");
        impl_->vkDevice.destroyImage(impl_->intermediateImg);
        impl_->intermediateImg = nullptr;
        return false;
    }
    impl_->vkDevice.bindImageMemory(impl_->intermediateImg, impl_->intermediateMemory, 0);
    impl_->intermediateLayout = vk::ImageLayout::eUndefined;

    return true;
}

void IPreviewer::releaseIntermediateTarget() {
    if (impl_->intermediateMemory) {
        impl_->vkDevice.freeMemory(impl_->intermediateMemory);
        impl_->intermediateMemory = nullptr;
    }
    if (impl_->intermediateImg) {
        impl_->vkDevice.destroyImage(impl_->intermediateImg);
        impl_->intermediateImg = nullptr;
    }
    impl_->intermediateLayout = vk::ImageLayout::eUndefined;
}

static inline VkImage      toC(vk::Image img)       { return static_cast<VkImage>(img); }
static inline VkSemaphore  toC(vk::Semaphore sem)   { return static_cast<VkSemaphore>(sem); }
static inline VkFormat     toC(vk::Format fmt)      { return static_cast<VkFormat>(fmt); }
static inline VkImageLayout toC(vk::ImageLayout l)  { return static_cast<VkImageLayout>(l); }

static inline VkImageUsageFlags toCUsage(vk::ImageUsageFlags u) {
    return static_cast<VkImageUsageFlags>(u);
}

bool IPreviewer::presentFrame(const AVFrame* avframe) {
    if (!impl_->initialized || !impl_->swapChain->isValid()) {
        return false;
    }

    int w = impl_->outputWidth;
    int h = impl_->outputHeight;
    if (w <= 0 || h <= 0) return false;

    const pl_frame* srcFrame = impl_->textureManager->uploadAvFrame(avframe);
    if (!srcFrame) return false;

    struct pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image  = toC(impl_->intermediateImg);
    wrapParams.width  = w;
    wrapParams.height = h;
    wrapParams.format = toC(vk::Format::eR8G8B8A8Unorm);
    wrapParams.usage  = toCUsage(vk::ImageUsageFlagBits::eStorage
                               | vk::ImageUsageFlagBits::eSampled
                               | vk::ImageUsageFlagBits::eColorAttachment);

    pl_tex targetTex = pl_vulkan_wrap(impl_->gpu, &wrapParams);
    if (!targetTex) {
        LOG_ERROR("IPreviewer: pl_vulkan_wrap() failed");
        return false;
    }

    pl_vulkan_release_params releaseP = {};
    releaseP.tex                = targetTex;
    releaseP.layout             = toC(impl_->intermediateLayout);
    releaseP.qf                 = VK_QUEUE_FAMILY_IGNORED;
    releaseP.semaphore.sem      = toC(impl_->releaseSemaphore);
    releaseP.semaphore.value    = 0;

    pl_vulkan_release_ex(impl_->gpu, &releaseP);

    pl_frame targetFrame = {};
    targetFrame.num_planes = 1;
    targetFrame.planes[0].texture    = targetTex;
    targetFrame.planes[0].components = 4;
    targetFrame.planes[0].component_mapping[0] = 0;
    targetFrame.planes[0].component_mapping[1] = 1;
    targetFrame.planes[0].component_mapping[2] = 2;
    targetFrame.planes[0].component_mapping[3] = 3;
    targetFrame.repr.sys        = PL_COLOR_SYSTEM_RGB;
    targetFrame.repr.levels     = PL_COLOR_LEVELS_PC;
    targetFrame.color.primaries = PL_COLOR_PRIM_BT_709;
    targetFrame.color.transfer  = PL_COLOR_TRC_SRGB;
    targetFrame.crop = { 0, 0, static_cast<float>(w), static_cast<float>(h) };

    if (!impl_->renderEngine->render(srcFrame, &targetFrame)) {
        LOG_WARN("IPreviewer: YUV→RGB render failed");
        pl_tex_destroy(impl_->gpu, &targetTex);
        return false;
    }

    pl_gpu_flush(impl_->gpu);

    pl_vulkan_hold_params holdP = {};
    holdP.tex                = targetTex;
    holdP.layout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    holdP.qf                 = VK_QUEUE_FAMILY_IGNORED;
    holdP.semaphore.sem      = toC(impl_->holdSemaphore);
    holdP.semaphore.value    = 0;

    if (!pl_vulkan_hold_ex(impl_->gpu, &holdP)) {
        LOG_ERROR("IPreviewer: hold after YUV→RGB failed");
        pl_tex_destroy(impl_->gpu, &targetTex);
        return false;
    }

    pl_tex_destroy(impl_->gpu, &targetTex);
    impl_->intermediateLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    VkImage dagOutputVk = toC(impl_->intermediateImg);
    int dagOutputW = w;
    int dagOutputH = h;

    if (impl_->filterGraph && impl_->dagInput && impl_->dagOutput) {
        impl_->dagInput->inputGpuData(nullptr, dagOutputVk);
        if (!impl_->filterGraph->run()) {
            LOG_WARN("IPreviewer: DAG filter graph run() failed — passthrough");
        } else {
            filtergraph::VkOutGpuTex outTex;
            impl_->dagOutput->outVkGpuTex(outTex, 0);
            if (outTex.image) {
                dagOutputVk = static_cast<VkImage>(outTex.image);
                dagOutputW  = outTex.width;
                dagOutputH  = outTex.height;
            }
        }
    }

    bool result = renderToSwapChain(dagOutputVk, dagOutputW, dagOutputH);

    if (impl_->onPresent) {
        impl_->onPresent();
    }

    return result;
}

bool IPreviewer::renderToSwapChain(VkImage image, int width, int height) {
    if (!image) return false;

    struct pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image  = image;
    wrapParams.width  = width;
    wrapParams.height = height;
    wrapParams.format = VK_FORMAT_R8G8B8A8_UNORM;
    wrapParams.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    pl_tex srcTex = pl_vulkan_wrap(impl_->gpu, &wrapParams);
    if (!srcTex) {
        LOG_ERROR("IPreviewer: pl_vulkan_wrap() failed for DAG output");
        return false;
    }

    pl_vulkan_release_params releaseP = {};
    releaseP.tex                = srcTex;
    releaseP.layout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    releaseP.qf                 = VK_QUEUE_FAMILY_IGNORED;
    releaseP.semaphore.sem      = toC(impl_->releaseSemaphore);
    releaseP.semaphore.value    = 0;

    pl_vulkan_release_ex(impl_->gpu, &releaseP);

    pl_tex fbo = impl_->swapChain->startFrame(width, height);
    if (!fbo) {
        pl_tex_destroy(impl_->gpu, &srcTex);
        return false;
    }

    pl_frame srcPlFrame = {};
    srcPlFrame.num_planes = 1;
    srcPlFrame.planes[0].texture    = srcTex;
    srcPlFrame.planes[0].components = 4;
    srcPlFrame.planes[0].component_mapping[0] = 0;
    srcPlFrame.planes[0].component_mapping[1] = 1;
    srcPlFrame.planes[0].component_mapping[2] = 2;
    srcPlFrame.planes[0].component_mapping[3] = 3;
    srcPlFrame.repr.sys        = PL_COLOR_SYSTEM_RGB;
    srcPlFrame.repr.levels     = PL_COLOR_LEVELS_PC;
    srcPlFrame.color.primaries = PL_COLOR_PRIM_BT_709;
    srcPlFrame.color.transfer  = PL_COLOR_TRC_SRGB;
    srcPlFrame.crop = { 0, 0, static_cast<float>(width), static_cast<float>(height) };

    pl_frame targetPlFrame = {};
    targetPlFrame.num_planes = 1;
    targetPlFrame.planes[0].texture    = fbo;
    targetPlFrame.planes[0].components = 4;
    targetPlFrame.planes[0].component_mapping[0] = 0;
    targetPlFrame.planes[0].component_mapping[1] = 1;
    targetPlFrame.planes[0].component_mapping[2] = 2;
    targetPlFrame.planes[0].component_mapping[3] = 3;
    targetPlFrame.repr.sys        = PL_COLOR_SYSTEM_RGB;
    targetPlFrame.repr.levels     = PL_COLOR_LEVELS_PC;
    targetPlFrame.color.primaries = PL_COLOR_PRIM_BT_709;
    targetPlFrame.color.transfer  = PL_COLOR_TRC_SRGB;
    targetPlFrame.crop = { 0, 0, static_cast<float>(width), static_cast<float>(height) };

    if (!impl_->renderEngine->render(&srcPlFrame, &targetPlFrame)) {
        LOG_WARN("IPreviewer: final render (DAG→FBO) failed");
        pl_tex_destroy(impl_->gpu, &srcTex);
        return false;
    }

    pl_tex_destroy(impl_->gpu, &srcTex);

    if (!impl_->swapChain->submitFrame()) {
        LOG_WARN("IPreviewer: swapchain submitFrame failed");
        return false;
    }
    impl_->swapChain->swapBuffers();

    return true;
}

} // namespace renderer
} // namespace heisenberg
