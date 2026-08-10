//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

#include <Controller/PlaybackController.hpp>
#include <IPreviewer.hpp>
#include <Renderer/GpuContext.hpp>
#include <Renderer/VulkanContext.hpp>
#include <Renderer/SwapChain.hpp>
#include <FilterGraph/Vulkan/VulkanFilterGraph.hpp>
#include <MainWidget/VideoWidget.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent) {
    previewer_ = std::make_unique<renderer::IPreviewer>();
}

PlayerController::~PlayerController() {
    shutdown();
}

void PlayerController::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    if (previewer_) {
        previewer_->setFilterGraph(nullptr, nullptr, nullptr);
    }
    filterGraph_.reset();
    if (previewer_) {
        previewer_->shutdown();
        previewer_.reset();
    }
    gpuCtx_.reset();

    LOG_INFO("PlayerController: GPU resources released");
}

// ============================================================
// 依赖注入
// ============================================================

void PlayerController::setPlaybackController(ctrl::PlaybackController* ctrl) {
    ctrl_ = ctrl;
    if (!ctrl_) return;

    connect(ctrl_, &ctrl::PlaybackController::frameDecoded,
            this, &PlayerController::onFrameDecoded);

    connect(ctrl_, &ctrl::PlaybackController::positionChanged,
            this, [this](double seconds) {
        currentTime_ = seconds;
        emit currentTimeChanged();
    });

    connect(ctrl_, &ctrl::PlaybackController::stateChanged,
            this, [this](ctrl::PlaybackController::State s) {
        bool playing = (s == ctrl::PlaybackController::Playing);
        if (isPlaying_ != playing) {
            isPlaying_ = playing;
            emit isPlayingChanged();
        }
    });

    connect(ctrl_, &ctrl::PlaybackController::durationChanged,
            this, [this](double d) {
        duration_ = d;
        frameCount_ = ctrl_ ? ctrl_->frameCount() : 0;
        emit durationChanged();
        emit frameCountChanged();
    });

    connect(ctrl_, &ctrl::PlaybackController::endOfStream,
            this, [this]() {
        if (isPlaying_) {
            isPlaying_ = false;
            emit isPlayingChanged();
        }
        if (currentTime_ != duration_) {
            currentTime_ = duration_;
            emit currentTimeChanged();
        }
    });
}

// ============================================================
// VideoWidget 绑定 + 管线初始化
// ============================================================

void PlayerController::bindVideoOutput(VideoWidget* widget) {
    if (!widget) return;
    videoOutput_ = widget;
    initPipeline(widget);
}

void PlayerController::initPipeline(VideoWidget* widget) {
    HWND hwnd = widget->nativeWindow();
    if (!hwnd) {
        LOG_ERROR("PlayerController: VideoWidget has no native window");
        return;
    }

    // ---- 从 VulkanContext 获取独立 Vulkan 资源 ----
    auto& vkCtx = renderer::VulkanContext::instance();
    auto vkInst    = vkCtx.vkInstance();
    auto vkPhysDev = vkCtx.physicalDevice();
    auto vkDev     = vkCtx.device();
    uint32_t qf    = vkCtx.graphicsQueueFamily();
    auto vkQueue   = vkCtx.graphicsQueue();

    // ---- 构建 GpuContext ----
    renderer::VulkanResources vkRes;
    vkRes.instance      = vkInst;
    vkRes.physDevice    = vkPhysDev;
    vkRes.device        = vkDev;
    vkRes.graphicsQF    = qf;
    vkRes.graphicsQueue = vkQueue;
    vkRes.getProcAddr   = vkCtx.getInstanceProcAddr();

    try {
        gpuCtx_ = std::make_unique<renderer::GpuContext>(vkRes);
    } catch (const std::exception& e) {
        LOG_ERROR("PlayerController: GpuContext creation failed — {}", e.what());
        return;
    }

    // ---- 创建 SwapChain ----
    int w = widget->width()  > 0 ? widget->width()  : 640;
    int h = widget->height() > 0 ? widget->height() : 360;

    auto swapChain = std::make_unique<renderer::SwapChain>();
    if (!swapChain->initialize(gpuCtx_->plVulkan(), vkInst, hwnd, w, h)) {
        LOG_ERROR("PlayerController: SwapChain initialization failed");
        gpuCtx_.reset();
        return;
    }

    // ---- 初始化 IPreviewer ----
    bool ok = previewer_->initialize(gpuCtx_->plGpu(), gpuCtx_->plVulkan(),
                                      std::move(swapChain), w, h);
    if (!ok) {
        LOG_ERROR("PlayerController: IPreviewer::initialize() failed");
        gpuCtx_.reset();
        return;
    }

    filtergraph::VulkanGraphContext graphContext;
    graphContext.instance = static_cast<VkInstance>(vkInst);
    graphContext.physicalDevice = static_cast<VkPhysicalDevice>(vkPhysDev);
    graphContext.device = static_cast<VkDevice>(vkDev);
    graphContext.queue = static_cast<VkQueue>(vkQueue);
    graphContext.queueFamilyIndex = qf;
    try {
        filterGraph_ = std::make_unique<filtergraph::VulkanFilterGraph>(
            graphContext);
        filterGraph_->exposure()->updateParamet(-2.0f);
        filterGraph_->blend()->updateParamet(0.5f);
        filterGraph_->gaussianBlur()->updateParamet({12, 0.0f});
        previewer_->setFilterGraph(filterGraph_->graph(), filterGraph_->input(),
                                   filterGraph_->output());
    } catch (const std::exception& error) {
        LOG_ERROR("PlayerController: filter graph initialization failed: {}",
                  error.what());
        filterGraph_.reset();
    }

    // ---- 尺寸跟随 ----
    connect(widget, &VideoWidget::windowResized, this, [this](int w, int h) {
        if (previewer_) {
            previewer_->resize(w, h);
            if (lastFrame_) {
                previewer_->presentFrame(lastFrame_.get());
            }
        }
    });

    previewer_->setOnResize([this](int w, int h) {
        videoWidth_  = w;
        videoHeight_ = h;
    });

    LOG_INFO("PlayerController: Vulkan pipeline initialized — HWND=0x{:x} {}x{}",
             reinterpret_cast<uintptr_t>(hwnd), w, h);
}

// ============================================================
// 帧回调
// ============================================================

void PlayerController::onFrameDecoded(std::shared_ptr<AVFrame> frame) {
    if (!frame || !frame->data[0]) return;
    if (!previewer_) return;

    lastFrame_ = frame;

    if (videoWidth_ <= 0 || videoHeight_ <= 0) {
        videoWidth_  = frame->width;
        videoHeight_ = frame->height;
    }

    previewer_->presentFrame(frame.get());
}

// ============================================================
// 文件加载
// ============================================================

bool PlayerController::openFile(const QString& path) {
    if (!ctrl_) return false;

    bool ok = ctrl_->open(path.toStdString());
    if (ok) {
        currentFile_ = path;
        emit currentFileChanged();
    }
    return ok;
}

void PlayerController::closeFile() {
    if (ctrl_) ctrl_->close();
    currentFile_.clear();
    currentTime_ = 0.0;
    duration_    = 0.0;
    frameCount_  = 0;
    isSeekable_  = false;
    isPlaying_   = false;
    videoWidth_  = 0;
    videoHeight_ = 0;
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
    emit frameCountChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
}

// ============================================================
// 播放控制
// ============================================================

void PlayerController::play()             { if (ctrl_) ctrl_->play(); }
void PlayerController::pause()            { if (ctrl_) ctrl_->pause(); }
void PlayerController::togglePlayPause()  { if (ctrl_) ctrl_->togglePlayPause(); }
void PlayerController::seek(double s)     { if (ctrl_) ctrl_->seek(s); }
void PlayerController::beginScrub()       { if (ctrl_) ctrl_->beginScrub(); }
void PlayerController::scrubToFrame(qint64 frame) {
    if (ctrl_) ctrl_->scrubToFrame(frame);
}
void PlayerController::endScrub(qint64 frame) {
    if (ctrl_) ctrl_->endScrub(frame);
}
void PlayerController::stepForward(int n) { if (ctrl_) ctrl_->stepForward(n); }
void PlayerController::stepBackward(int n){ if (ctrl_) ctrl_->stepBackward(n); }
void PlayerController::goToStart()        { if (ctrl_) ctrl_->goToStart(); }
void PlayerController::goToEnd()          { if (ctrl_) ctrl_->goToEnd(); }

} // namespace ui
} // namespace heisenberg
