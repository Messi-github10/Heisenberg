//
// Created by NiceFold on 2026/7/9.
//

#include "IPreviewer.hpp"
#include "Renderer/RenderEngine.hpp"
#include "TextureContext/TextureManager.hpp"
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

    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<RenderEngine>   renderEngine;

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

    // 1. TextureManager（上传 + 目标管理 + 纹理池）
    try {
        impl_->textureManager = std::make_unique<TextureManager>(gpu, device, queueFamily);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: TextureManager creation failed — {}", e.what());
        return false;
    }

    // 2. RenderEngine
    try {
        impl_->renderEngine = std::make_unique<RenderEngine>(gpu);
    } catch (const std::exception& e) {
        LOG_ERROR("IPreviewer: RenderEngine creation failed — {}", e.what());
        impl_->textureManager.reset();
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
    const pl_frame* srcFrame = impl_->textureManager->uploadAvFrame(avframe);
    if (!srcFrame) {
        return {};
    }

    // 2. 获取渲染目标
    auto target = impl_->textureManager->acquireTarget(w, h);
    if (!target.tex || !target.frame) {
        LOG_WARN("IPreviewer: acquireTarget failed");
        return {};
    }

    // 3. 渲染
    bool ok = impl_->renderEngine->render(srcFrame, target.frame);
    if (!ok) {
        LOG_WARN("IPreviewer: render failed");
        impl_->textureManager->discardTarget(target.tex, w, h);
        return {};
    }

    // 4. Finalize → VkImage
    vk::Image vkImage = impl_->textureManager->finalizeAndExport(target.tex, w, h);
    if (!vkImage) {
        LOG_ERROR("IPreviewer: finalizeAndExport failed");
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

void IPreviewer::recycleVkImage(vk::Image image) {
    if (image && impl_->textureManager) {
        impl_->textureManager->recycleImage(image);
    }
}

} // namespace renderer
} // namespace heisenberg
