#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

extern "C" {
struct AVFrame;
}

namespace heisenberg {
namespace ctrl {

class PlaybackController {
public:
    enum State {
        Idle,
        Loading,
        Playing,
        Paused,
        Scrubbing,
        Ended
    };

    using Task = std::function<void()>;
    using TaskDispatcher = std::function<void(Task)>;
    using FramePtr = std::shared_ptr<AVFrame>;

    PlaybackController();
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void setTaskDispatcher(TaskDispatcher dispatcher);

    bool open(const std::string& filePath);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void beginScrub();
    void scrubToFrame(int64_t frameIndex);
    void endScrub(int64_t frameIndex);
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
    int64_t frameCount() const;
    void setHardwareDecode(bool enabled);

    std::function<void(State)> onStateChanged;
    std::function<void(FramePtr)> onFrameDecoded;
    std::function<void(double)> onPositionChanged;
    std::function<void(double)> onDurationChanged;
    std::function<void()> onEndOfStream;
    std::function<void(const std::string&)> onOpenFailed;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void setState(State s);
    void dispatch(Task task);
};

} // namespace ctrl
} // namespace heisenberg
