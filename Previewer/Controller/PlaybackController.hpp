//
// Created by NiceFold on 2026/7/7.
//

#pragma once

#include <QObject>
#include <memory>

extern "C" {
struct AVFrame;
}

namespace heisenberg {
namespace ctrl {

class PlaybackController : public QObject {
    Q_OBJECT
public:
    enum State {
        Idle,
        Playing,
        Paused,
        Ended
    };

    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    bool open(const std::string& filePath);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    State state() const;
    bool isPlaying() const;
    double currentTime() const;
    double duration() const;
    bool isSeekable() const;
    double fps() const;

signals:
    void stateChanged(heisenberg::ctrl::PlaybackController::State newState);
    void frameDecoded(std::shared_ptr<AVFrame> frame);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void endOfStream();

private slots:
    void onTick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void setState(State s);
    std::shared_ptr<::AVFrame> decodeFrameForTarget(double targetPtsMs);
    void scheduleNextTick(double targetPtsMs);
    void resetClock(double startPtsMs);
    void decodeFirstFrame();
};

} // namespace ctrl
} // namespace heisenberg
