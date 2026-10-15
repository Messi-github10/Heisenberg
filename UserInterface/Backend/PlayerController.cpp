#include "PlayerController.hpp"
#include "EditorSession.hpp"

#include <Compositing/CompositionBuilder.hpp>
#include <MainWidget/VideoWidget.hpp>
#include <Utiles/Logger.hpp>

#include <QMetaObject>

#include <utility>

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(EditorSession* session, QObject* parent)
    : QObject(parent)
    , previewer_(heisenberg::IPreviewer::create())
    , previewerAlive_(std::make_shared<std::atomic<bool>>(true))
    , session_(session) {
    previewer_->setTaskDispatcher(
        [this, alive = previewerAlive_](heisenberg::IPreviewer::Task task) {
            QMetaObject::invokeMethod(this, [alive, task = std::move(task)]() mutable {
                if (!alive->load() || !task) return;
                task();
            }, Qt::QueuedConnection);
        });
    previewer_->setListener(this);
    connect(session_, &EditorSession::projectChanged, this, [this]() { timelineTime_ = 0.0; });
    connect(session_, &EditorSession::timelineChanged, this, &PlayerController::showTimeline);
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

std::optional<filtergraph::VulkanGraphDocument> PlayerController::filterGraphSnapshot() const {
    if (!previewer_) return std::nullopt;
    auto graph = previewer_->filterGraphSnapshot();
    if (graph.nodes().empty() && graph.edges().empty()) return std::nullopt;
    return graph;
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

bool PlayerController::openFile(const QString& path) { return openSource(path); }

bool PlayerController::openSource(const QString& path, const std::string& mediaRef) {
    if (!previewer_) return false;
    pendingPosition_ = -1.0;
    scrubRequested_ = false;
    pendingScrubFrame_ = -1;
    previewingSource_ = true;
    emit previewModeChanged(true, path);
    currentTime_ = 0.0;
    duration_ = 0.0;
    frameCount_ = 0;
    isSeekable_ = false;
    isPlaying_ = false;
    emit currentTimeChanged();
    emit durationChanged();
    emit frameCountChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
    previewer_->openSource(path.toUtf8().toStdString(), session_->editor().project().colorSettings, mediaRef);
    currentFile_ = path;
    emit currentFileChanged();
    return true;
}

bool PlayerController::openPlaylist(const QString& path) {
    QString error;
    if (session_->loadPackage(path, &error)) return true;
    emit playbackFailed(error);
    return false;
}

void PlayerController::previewAsset(const QString& mediaRef) {
    const auto asset = session_->editor().resolver().asset(mediaRef.toStdString());
    if (asset.missing) { emit playbackFailed(tr("This media is offline.")); return; }
    openSource(QString::fromUtf8(asset.path.c_str()), mediaRef.toUtf8().toStdString());
}

void PlayerController::showTimeline(qint64 focusFrame) {
    if (!previewer_) return;
    const auto snapshot = session_->snapshot();
    const auto* timeline = snapshot.activeTimeline();
    previewingSource_ = false;
    emit previewModeChanged(false, timeline ? QString::fromUtf8(timeline->name.c_str()) : tr("Timeline"));
    scrubRequested_ = false;
    pendingScrubFrame_ = -1;
    if (focusFrame >= 0 && timeline) timelineTime_ = static_cast<double>(focusFrame) / timeline->fps;
    pendingPosition_ = timelineTime_;
    currentTime_ = timelineTime_;
    duration_ = timeline ? static_cast<double>(timeline->totalFrames()) / timeline->fps : 0.0;
    frameCount_ = timeline ? timeline->totalFrames() : 0;
    emit durationChanged();
    emit frameCountChanged();
    emit currentTimeChanged();
    if (!timeline || timeline->totalFrames() == 0) {
        pendingPosition_ = -1.0;
        closeFile();
        return;
    }
    CompositionPlan plan;
    std::string error;
    if (!CompositionBuilder::build(snapshot, plan, &error)) {
        pendingPosition_ = -1.0;
        emit playbackFailed(QString::fromUtf8(error.c_str()));
        return;
    }
    previewer_->openComposition(plan);
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
void PlayerController::seek(double seconds) {
    if (!previewer_) return;
    // A newer explicit selection supersedes the initial timeline focus. Do not
    // let the asynchronous open's Paused notification seek back to that focus.
    if (previewer_->state() == IPreviewer::State::Loading) {
        pendingPosition_ = seconds;
        return;
    }
    pendingPosition_ = -1.0;
    previewer_->seek(seconds);
}
void PlayerController::beginScrub() {
    if (!previewer_) return;
    if (previewingSource_) showTimeline();
    scrubRequested_ = true;
    if (previewer_->state() != IPreviewer::State::Loading) previewer_->beginScrub();
}
void PlayerController::scrubToFrame(qint64 frame) {
    if (!previewer_) return;
    pendingScrubFrame_ = frame;
    if (previewer_->state() == IPreviewer::State::Loading) return;
    previewer_->scrubToFrame(frame);
}
void PlayerController::endScrub(qint64 frame) {
    scrubRequested_ = false;
    pendingScrubFrame_ = -1;
    if (!previewer_) return;
    if (previewer_->state() == IPreviewer::State::Loading) {
        const auto* timeline = session_->editor().activeTimeline();
        if (timeline) pendingPosition_ = static_cast<double>(frame) / timeline->fps;
        return;
    }
    previewer_->endScrub(frame);
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
    if (state == IPreviewer::State::Paused && pendingPosition_ >= 0.0) {
        const double position = pendingPosition_;
        pendingPosition_ = -1.0;
        previewer_->seek(position);
        return;
    }
    if (state == IPreviewer::State::Paused && scrubRequested_) {
        previewer_->beginScrub();
        if (pendingScrubFrame_ >= 0) previewer_->scrubToFrame(pendingScrubFrame_);
    }
}

void PlayerController::onPositionChanged(double seconds) {
    currentTime_ = seconds;
    if (!previewingSource_) timelineTime_ = seconds;
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
    pendingPosition_ = -1.0;
    scrubRequested_ = false;
    emit playbackFailed(QString::fromUtf8(reason.c_str()));
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
