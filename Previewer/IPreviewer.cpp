//
// Created by NiceFold on 2026/7/9.
//

#include "IPreviewer.hpp"
#include "Renderer/RenderEngine.hpp"
#include "Renderer/RenderTarget.hpp"
#include "Renderer/TextureTransfer.hpp"
#include <volk.h>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace renderer {

struct IPreviewer::Impl {
    pl_gpu     gpu    = nullptr;
    vk::Device device;

    std::unique_ptr<RenderTarget>    renderTarget;
    std::unique_ptr<RenderEngine>    renderEngine;
    std::unique_ptr<TextureTransfer> textureTransfer;

    ResizeCallback  onResize;
    PresentCallback onPresent;

    int outputWidth  = 0;
    int outputHeight = 0;
    bool initialized = false;
};

IPreviewer::IPreviewer()
    : impl_(std::make_unique<Impl>()) {}

IPreviewer::~IPreviewer() = default;

bool IPreviewer::initialize(pl_gpu gpu, vk::Device device,
                            uint32_t queueFamily, int width, int height) {
    if (!gpu || !device) {
        LOG_ERROR("IPreviewer: invalid parameters");
        return false;
    }

    impl_->gpu          = gpu;
    impl_->device       = device;
    impl_->outputWidth  = width;
    impl_->outputHeight = height;

    // 1. RenderTarget
    try {
        impl_->renderTarget = std::make_unique<RenderTarget>(gpu, queueFamily);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: RenderTarget creation failed — {}", e.what());
        return false;
    }

    // 2. RenderEngine
    try {
        impl_->renderEngine = std::make_unique<RenderEngine>(gpu);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: RenderEngine creation failed — {}", e.what());
        return false;
    }

    // 3. TextureTransfer
    try {
        impl_->textureTransfer = std::make_unique<TextureTransfer>(gpu);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: TextureTransfer creation failed — {}", e.what());
        impl_->renderEngine.reset();
        return false;
    }

    impl_->initialized = true;
    LOG_INFO("IPreviewer: initialized — {}x{}", width, height);
    return true;
}

FrameOutput IPreviewer::presentFrame(const AVFrame* avframe) {
    if (!impl_->initialized) {
        LOG_WARN("IPreviewer: presentFrame called but not initialized");
        return {};
    }

    int w = impl_->outputWidth;
    int h = impl_->outputHeight;
    if (w <= 0 || h <= 0) {
        LOG_WARN("IPreviewer: invalid output size — {}x{}", w, h);
        return {};
    }

    // 1. 上传源帧
    const pl_frame* srcFrame = impl_->textureTransfer->uploadAVFrame(avframe);
    if (!srcFrame) {
        return {};
    }

    // 2. 确保目标纹理尺寸正确
    impl_->renderTarget->resize(w, h);

    // 3. 渲染
    bool ok = impl_->renderEngine->render(srcFrame,
                                           impl_->renderTarget->targetFrame());
    if (!ok) {
        LOG_WARN("IPreviewer: render failed");
        return {};
    }

    // 4. finalize
    vk::Image vkImage = impl_->renderTarget->finalize();
    if (!vkImage) {
        LOG_ERROR("IPreviewer: RenderTarget::finalize() returned null");
        return {};
    }

    if (impl_->onPresent) {
        impl_->onPresent();
    }

    return FrameOutput{ vkImage, vk::ImageLayout::eShaderReadOnlyOptimal, w, h };
}

void IPreviewer::resize(int width, int height) {
    impl_->outputWidth  = width;
    impl_->outputHeight = height;
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

void IPreviewer::destroyVkImage(vk::Image image) {
    if (image && impl_->device) {
        impl_->device.destroyImage(image);
    }
}

} // namespace renderer
} // namespace heisenberg
