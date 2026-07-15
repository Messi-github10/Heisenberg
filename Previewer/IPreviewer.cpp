//
// Created by NiceFold on 2026/7/9.
//

#include "IPreviewer.hpp"
#include "Renderer/RenderEngine.hpp"
#include "Renderer/SwapChain.hpp"
#include "TextureContext/TextureManager.hpp"

#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libplacebo/renderer.h>
}

namespace heisenberg {
namespace renderer {

struct IPreviewer::Impl {
    pl_gpu gpu = nullptr;

    std::unique_ptr<TextureManager> textureManager;
    std::unique_ptr<RenderEngine>   renderEngine;
    std::unique_ptr<SwapChain>      swapChain;

    ResizeCallback  onResize;
    PresentCallback onPresent;

    int outputWidth  = 0;
    int outputHeight = 0;
    bool initialized = false;
};

IPreviewer::IPreviewer()
    : impl_(std::make_unique<Impl>()) {}

IPreviewer::~IPreviewer() {
    shutdown();
}

bool IPreviewer::initialize(pl_gpu gpu,
                            std::unique_ptr<SwapChain> swapChain,
                            int width, int height) {
    if (!gpu || !swapChain || !swapChain->isValid()) {
        LOG_ERROR("IPreviewer: invalid parameters");
        return false;
    }

    impl_->gpu          = gpu;
    impl_->swapChain    = std::move(swapChain);
    impl_->outputWidth  = width;
    impl_->outputHeight = height;

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

    impl_->initialized = true;
    LOG_INFO("IPreviewer: initialized — {}x{}", width, height);
    return true;
}

bool IPreviewer::presentFrame(const AVFrame* avframe) {
    if (!impl_->initialized || !impl_->swapChain->isValid()) {
        return false;
    }

    int w = impl_->outputWidth;
    int h = impl_->outputHeight;
    if (w <= 0 || h <= 0) return false;

    // 1. 上传源帧
    const pl_frame* srcFrame = impl_->textureManager->uploadAvFrame(avframe);
    if (!srcFrame) return false;

    // 2. 从 swapchain 获取 framebuffer
    pl_tex target = impl_->swapChain->startFrame(w, h);
    if (!target) return false;

    // 3. 构建目标帧
    pl_frame targetFrame = {};
    targetFrame.num_planes = 1;
    targetFrame.planes[0].texture    = target;
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

    // 4. 渲染
    if (!impl_->renderEngine->render(srcFrame, &targetFrame)) {
        LOG_WARN("IPreviewer: render failed");
        return false;
    }

    // 5. 提交 + 呈现
    if (!impl_->swapChain->submitFrame()) {
        LOG_WARN("IPreviewer: swapchain submitFrame failed");
        return false;
    }
    impl_->swapChain->swapBuffers();

    if (impl_->onPresent) {
        impl_->onPresent();
    }

    return true;
}

void IPreviewer::resize(int width, int height) {
    impl_->outputWidth  = width;
    impl_->outputHeight = height;
    if (impl_->swapChain) {
        int w = width, h = height;
        impl_->swapChain->resize(&w, &h);
    }
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

void IPreviewer::shutdown() {
    if (impl_->swapChain) {
        impl_->swapChain->shutdown();
    }
    if (impl_->textureManager) {
        impl_->textureManager->shutdown();
    }
    impl_->renderEngine.reset();
    impl_->initialized = false;
}

} // namespace renderer
} // namespace heisenberg
