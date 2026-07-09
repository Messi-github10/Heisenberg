//
// Created by NiceFold on 2026/7/9.
//

#include "VideoPreviewer.hpp"

#include "GpuContext.hpp"
#include "SwapChain.hpp"
#include "RenderEngine.hpp"
#include "TextureTransfer.hpp"
#include "VulkanContext.hpp"

#include <Utiles/Logger.hpp>

namespace heisenberg {
namespace render {

struct VideoPreviewer::Impl {
    std::unique_ptr<GpuContext>      gpuCtx;           // 需最后析构，声明在最前
    std::unique_ptr<SwapChain>       swapChain;
    std::unique_ptr<RenderEngine>    renderEngine;
    std::unique_ptr<TextureTransfer> textureTransfer;

    ResizeCallback  onResize;
    PresentCallback onPresent;

    bool initialized = false;
};

VideoPreviewer::VideoPreviewer()
    : impl_(std::make_unique<Impl>()) {}

VideoPreviewer::~VideoPreviewer() = default;

bool VideoPreviewer::initialize(HWND hwnd, int width, int height) {
    if (!hwnd) {
        LOG_ERROR("VideoPreviewer: invalid HWND");
        return false;
    }

    // 1. GpuContext（封装 pl_vulkan）
    try {
        impl_->gpuCtx = std::make_unique<GpuContext>();
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: GpuContext creation failed — {}", e.what());
        return false;
    }

    // 2. 从子 HWND 创建 VkSurfaceKHR
    auto& vkCtx = VulkanContext::instance();
    VkSurfaceKHR surface = vkCtx.createSurface(hwnd);
    if (surface == VK_NULL_HANDLE) {
        LOG_ERROR("VideoPreviewer: createSurface() failed");
        impl_->gpuCtx.reset();
        return false;
    }

    // 3. SwapChain（接管 surface 所有权）
    try {
        impl_->swapChain = std::make_unique<SwapChain>(
            impl_->gpuCtx->vulkan(), surface, width, height);
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: SwapChain creation failed — {}", e.what());
        impl_->gpuCtx.reset();
        return false;
    }

    // 4. RenderEngine
    try {
        impl_->renderEngine = std::make_unique<RenderEngine>(impl_->gpuCtx->gpu());
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: RenderEngine creation failed — {}", e.what());
        impl_->swapChain.reset();
        impl_->gpuCtx.reset();
        return false;
    }

    // 5. TextureTransfer
    try {
        impl_->textureTransfer = std::make_unique<TextureTransfer>(impl_->gpuCtx->gpu());
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: TextureTransfer creation failed — {}", e.what());
        impl_->renderEngine.reset();
        impl_->swapChain.reset();
        impl_->gpuCtx.reset();
        return false;
    }

    // 首次 resize 创建实际 swapchain
    int w = width, h = height;
    impl_->swapChain->resize(&w, &h);
    LOG_INFO("VideoPreviewer: swapchain resized to {}x{}", w, h);

    impl_->initialized = true;
    LOG_INFO("VideoPreviewer: initialized — {}x{}", width, height);
    return true;
}

bool VideoPreviewer::presentFrame(const AVFrame* avframe) {
    if (!impl_->initialized) {
        LOG_WARN("VideoPreviewer: presentFrame called but not initialized");
        return false;
    }

    // 1. 开始帧（可能因窗口隐藏/恢复导致 swapchain 失效）
    if (!impl_->swapChain->startFrame()) {
        // 尝试重建 swapchain
        int w = 640, h = 360;
        if (impl_->swapChain->resize(&w, &h)) {
            // 重试
            if (!impl_->swapChain->startFrame()) {
                return false;
            }
        } else {
            return false;
        }
    }

    // 2. 上传 AVFrame → GPU 纹理
    const pl_frame* srcFrame = impl_->textureTransfer->uploadAVFrame(avframe);
    if (!srcFrame) {
        impl_->swapChain->submitFrame();
        return false;
    }

    // 3. 渲染（GPU YUV→RGB）
    const pl_frame* target = impl_->swapChain->getTargetFrame();
    bool ok = impl_->renderEngine->render(srcFrame, target);

    // 4. 提交帧
    impl_->swapChain->submitFrame();

    return ok;
}

void VideoPreviewer::resize(int width, int height) {
    if (!impl_->initialized || !impl_->swapChain) {
        return;
    }

    int w = width, h = height;
    if (impl_->swapChain->resize(&w, &h)) {
        LOG_DEBUG("VideoPreviewer: resized to {}x{}", w, h);
        if (impl_->onResize) {
            impl_->onResize(w, h);
        }
    }
}

void VideoPreviewer::setOnResize(ResizeCallback cb) {
    impl_->onResize = std::move(cb);
}

void VideoPreviewer::setOnPresent(PresentCallback cb) {
    impl_->onPresent = std::move(cb);
}

} // namespace render
} // namespace heisenberg
