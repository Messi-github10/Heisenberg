//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

#include <Controller/PlaybackController.hpp>
#include <Utiles/Logger.hpp>

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent)
{
}

PlayerController::~PlayerController() = default;

// ============================================================
// 依赖注入
// ============================================================

void PlayerController::setPlaybackController(ctrl::PlaybackController* ctrl) {
    ctrl_ = ctrl;
    if (!ctrl_) return;

    // frameReady → 更新 FrameImageProvider + frameRevision
    connect(ctrl_, &ctrl::PlaybackController::frameReady,
            this, [this](const QImage& img) {
        if (imageProvider_) {
            imageProvider_->setFrame(img);
        }
        frameRevision_++;
        emit frameDecoded();  // 驱动 QML Image 刷新
    });

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

void PlayerController::setImageProvider(FrameImageProvider* provider) {
    imageProvider_ = provider;
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
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
}

// ============================================================
// 播放控制 — 全部转发到 PlaybackController
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
