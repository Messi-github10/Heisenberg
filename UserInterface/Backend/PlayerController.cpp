#include "PlayerController.hpp"

#include <MainWidget/VideoWidget.hpp>
#include <Utiles/Logger.hpp>

#include <QMetaObject>

#include <utility>

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent)
    , previewer_(heisenberg::IPreviewer::create())
    , previewerAlive_(std::make_shared<std::atomic<bool>>(true)) {
    previewer_->setTaskDispatcher(
        [this, alive = previewerAlive_](heisenberg::IPreviewer::Task task) {
            QMetaObject::invokeMethod(this, [alive, task = std::move(task)]() mutable {
                if (!alive->load() || !task) return;
                task();
            }, Qt::QueuedConnection);
        });
    previewer_->setListener(this);
}

PlayerController::~PlayerController() {
    shutdown();
}

void PlayerController::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    if (previewerAlive_) previewerAlive_->store(false);
    if (previewer_) {
        previewer_->setListener(nullptr);
        previewer_->shutdown();
        previewer_.reset();
    }

    LOG_INFO("PlayerController: GPU resources released");
}

void PlayerController::bindVideoOutput(VideoWidget* widget) {
    if (!widget || !previewer_) return;
    videoOutput_ = widget;

    const int width = widget->width() > 0 ? widget->width() : 640;
    const int height = widget->height() > 0 ? widget->height() : 360;
    previewer_->attachWindow(widget->nativeWindow(), width, height);

    connect(widget, &VideoWidget::windowResized, this, [this](int w, int h) {
        if (previewer_) previewer_->resize(w, h);
    });
}

void PlayerController::setHardwareDecode(bool enabled) {
    if (previewer_) previewer_->setHardwareDecode(enabled);
}

void PlayerController::openFilterGraph(const QString& path) {
    if (previewer_) previewer_->openFilterGraph(path.toStdString());
}

bool PlayerController::setFilterParameter(const QString& filterId,
                                          const QString& name,
                                          float value) {
    return previewer_
        && previewer_->setFilterParameter(filterId.toStdString(),
                                          name.toStdString(), value);
}

bool PlayerController::getFilterParameter(const QString& filterId,
                                          const QString& name,
                                          float& value) const {
    return previewer_
        && previewer_->getFilterParameter(filterId.toStdString(),
                                          name.toStdString(), value);
}

bool PlayerController::openFile(const QString& path) {
    if (!previewer_) return false;
    previewer_->open(path.toStdString());
    currentFile_ = path;
    emit currentFileChanged();
    return true;
}

bool PlayerController::openPlaylist(const QString& path) {
    if (!previewer_) return false;
    previewer_->openPlaylist(path.toStdString());
    currentFile_ = path;
    emit currentFileChanged();
    return true;
}

void PlayerController::closeFile() {
    if (previewer_) previewer_->close();
    currentFile_.clear();
    currentTime_ = 0.0;
    duration_    = 0.0;
    frameCount_  = 0;
    isSeekable_  = false;
    isPlaying_   = false;
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
    emit frameCountChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
}

void PlayerController::play()            { if (previewer_) previewer_->play(); }
void PlayerController::pause()           { if (previewer_) previewer_->pause(); }
void PlayerController::togglePlayPause() { if (previewer_) previewer_->togglePlayPause(); }
void PlayerController::seek(double s)    { if (previewer_) previewer_->seek(s); }
void PlayerController::beginScrub()      { if (previewer_) previewer_->beginScrub(); }
void PlayerController::scrubToFrame(qint64 frame) {
    if (previewer_) previewer_->scrubToFrame(frame);
}
void PlayerController::endScrub(qint64 frame) {
    if (previewer_) previewer_->endScrub(frame);
}
void PlayerController::stepForward(int n)  { if (previewer_) previewer_->stepForward(n); }
void PlayerController::stepBackward(int n) { if (previewer_) previewer_->stepBackward(n); }
void PlayerController::goToStart()         { if (previewer_) previewer_->goToStart(); }
void PlayerController::goToEnd()           { if (previewer_) previewer_->goToEnd(); }

void PlayerController::onStateChanged(heisenberg::IPreviewer::State state) {
    const bool playing = (state == heisenberg::IPreviewer::State::Playing);
    if (isPlaying_ != playing) {
        isPlaying_ = playing;
        emit isPlayingChanged();
    }
    if (!previewer_) return;
    const bool seekable = previewer_->isSeekable();
    if (isSeekable_ != seekable) {
        isSeekable_ = seekable;
        emit isSeekableChanged();
    }
}

void PlayerController::onPositionChanged(double seconds) {
    currentTime_ = seconds;
    emit currentTimeChanged();
}

void PlayerController::onDurationChanged(double seconds) {
    duration_ = seconds;
    frameCount_ = previewer_ ? previewer_->frameCount() : 0;
    emit durationChanged();
    emit frameCountChanged();
}

void PlayerController::onEndOfStream() {
    if (isPlaying_) {
        isPlaying_ = false;
        emit isPlayingChanged();
    }
    if (currentTime_ != duration_) {
        currentTime_ = duration_;
        emit currentTimeChanged();
    }
}

void PlayerController::onOpenFailed(const std::string& reason) {
    LOG_ERROR("PlayerController: open failed — {}", reason);
}

void PlayerController::onFilterGraphChanged(const std::string& path) {
    filterGraphPath_ = QString::fromStdString(path);
    emit filterGraphPathChanged();
}

void PlayerController::onFilterGraphFailed(const std::string& message) {
    LOG_ERROR("PlayerController: filter graph load failed: {}", message);
    emit filterGraphLoadFailed(QString::fromStdString(message));
}

} // namespace ui
} // namespace heisenberg
