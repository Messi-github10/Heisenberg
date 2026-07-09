//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

#include <Controller/PlaybackController.hpp>
#include <Render/VideoPreviewer.hpp>
#include <Backend/VideoOutputItem.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent) {
    previewer_ = std::make_unique<render::VideoPreviewer>();
}

PlayerController::~PlayerController() = default;

// ============================================================
// 依赖注入
// ============================================================

void PlayerController::setPlaybackController(ctrl::PlaybackController* ctrl) {
    ctrl_ = ctrl;
    if (!ctrl_) return;

    // frameDecoded → 渲染管线
    connect(ctrl_, &ctrl::PlaybackController::frameDecoded,
            this, &PlayerController::onFrameDecoded);

    // positionChanged → currentTime
    connect(ctrl_, &ctrl::PlaybackController::positionChanged,
            this, [this](double seconds) {
        currentTime_ = seconds;
        emit currentTimeChanged();
    });

    // stateChanged → isPlaying
    connect(ctrl_, &ctrl::PlaybackController::stateChanged,
            this, [this](ctrl::PlaybackController::State s) {
        bool playing = (s == ctrl::PlaybackController::Playing);
        if (isPlaying_ != playing) {
            isPlaying_ = playing;
            emit isPlayingChanged();
        }
    });

    // durationChanged
    connect(ctrl_, &ctrl::PlaybackController::durationChanged,
            this, [this](double d) {
        duration_ = d;
        emit durationChanged();
    });

    // endOfStream
    connect(ctrl_, &ctrl::PlaybackController::endOfStream,
            this, [this]() {
        if (isPlaying_) {
            isPlaying_ = false;
            emit isPlayingChanged();
        }
    });
}

// ============================================================
// VideoOutputItem 绑定
// ============================================================

void PlayerController::bindVideoOutput(VideoOutputItem* item) {
    if (!item) return;
    LOG_INFO("PlayerController: bindVideoOutput called, nativeWindow={}",
             reinterpret_cast<uintptr_t>(item->nativeWindow()));

    // 子 HWND 就绪 → 初始化管线
    connect(item, &VideoOutputItem::nativeWindowReady,
            this, &PlayerController::onNativeWindowReady,
            Qt::SingleShotConnection);

    // 尺寸变化 → 重建 swapchain
    auto syncSize = [this, item]() {
        qreal w = item->width();
        qreal h = item->height();
        if (w > 0 && h > 0) {
            previewer_->resize(static_cast<int>(w), static_cast<int>(h));
        }
    };
    connect(item, &QQuickItem::widthChanged,  this, syncSize);
    connect(item, &QQuickItem::heightChanged, this, syncSize);

    // 如果已就绪
    if (item->nativeWindow()) {
        onNativeWindowReady(item->nativeWindow());
    }
}

void PlayerController::onNativeWindowReady(HWND hwnd) {
    LOG_INFO("PlayerController: onNativeWindowReady hwnd=0x{:x}", reinterpret_cast<uintptr_t>(hwnd));

    // 延迟初始化，避免 Vulkan 设备创建干扰窗口启动的焦点
    QMetaObject::invokeMethod(this, [this, hwnd]() {
        int w = videoWidth_  > 0 ? videoWidth_  : 640;
        int h = videoHeight_ > 0 ? videoHeight_ : 360;

        bool ok = previewer_->initialize(hwnd, w, h);
        if (!ok) {
            LOG_ERROR("PlayerController: VideoPreviewer::initialize() failed");
            return;
        }

        previewer_->setOnResize([this](int width, int height) {
            videoWidth_  = width;
            videoHeight_ = height;
        });

        LOG_INFO("PlayerController: Vulkan pipeline bound to HWND 0x{:x}",
                 reinterpret_cast<uintptr_t>(hwnd));
    }, Qt::QueuedConnection);
}

void PlayerController::disconnectPreviewer() {
    // 预留：未来回收管线资源
}

// ============================================================
// 帧回调
// ============================================================

void PlayerController::onFrameDecoded(std::shared_ptr<AVFrame> frame) {
    if (!frame || !frame->data[0]) return;

    if (videoWidth_ <= 0 || videoHeight_ <= 0) {
        videoWidth_  = frame->width;
        videoHeight_ = frame->height;
        LOG_INFO("PlayerController: first frame decoded — {}x{}", videoWidth_, videoHeight_);
    }

    bool ok = previewer_->presentFrame(frame.get());
    if (!ok) {
        LOG_WARN("PlayerController: presentFrame failed");
    }
}

// ============================================================
// 文件加载
// ============================================================

bool PlayerController::openFile(const QString& path) {
    if (!ctrl_) return false;

    bool ok = ctrl_->open(path.toStdString());
    if (ok) {
        currentFile_ = path;
        duration_    = ctrl_->duration();
        isSeekable_  = ctrl_->isSeekable();
        emit currentFileChanged();
        emit durationChanged();
        emit isSeekableChanged();
    }
    return ok;
}

void PlayerController::closeFile() {
    if (ctrl_) ctrl_->close();
    currentFile_.clear();
    currentTime_ = 0.0;
    duration_    = 0.0;
    isSeekable_  = false;
    isPlaying_   = false;
    videoWidth_  = 0;
    videoHeight_ = 0;
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
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
void PlayerController::stepForward(int n) { if (ctrl_) ctrl_->stepForward(n); }
void PlayerController::stepBackward(int n){ if (ctrl_) ctrl_->stepBackward(n); }
void PlayerController::goToStart()        { if (ctrl_) ctrl_->goToStart(); }
void PlayerController::goToEnd()          { if (ctrl_) ctrl_->goToEnd(); }

} // namespace ui
} // namespace heisenberg
