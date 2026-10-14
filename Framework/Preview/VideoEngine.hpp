#pragma once

#include <Common/VideoFrame.hpp>
#include <Common/AudioFrame.hpp>
#include <Compositing/CompositionPlan.hpp>
#include <functional>
#include <memory>

namespace heisenberg {

struct Timeline;
class MediaManifest;

// Raw source image and intended presentation timing are deliberately separate.
// Native AVFrame timestamps are never rewritten by decode or timeline mapping.
struct DecodedFrame {
    std::shared_ptr<VideoFrame> source;
    TimelineSettings settings;
    int64_t targetPosition = 0;
    Rational targetTimeBase{1, 1};
    uint64_t generation = 0;
    bool sourceMode = false;
    bool blank = false;
};

class VideoEngine {
public:
    enum class State { Idle, Loading, Playing, Paused, Scrubbing, Ended };
    using Task = std::function<void()>;
    using TaskDispatcher = std::function<void(Task)>;
    struct Snapshot {
        State state = State::Idle;
        double currentTime = 0.0;
        double duration = 0.0;
        double fps = 0.0;
        int64_t frameCount = 0;
        bool seekable = false;
        bool hasVideo = false;
        bool audioOnly = false;
        uint64_t generation = 0;
        std::vector<std::string> warnings;
    };
    enum class Pump { Presented, Wait, Paused, Ended };

    VideoEngine();
    ~VideoEngine();
    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;
    void setTaskDispatcher(TaskDispatcher dispatcher);
    bool open(const std::string& path);
    bool openPlaylist(const std::string& path);
    bool openTimeline(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir);
    void close();
    void shutdown();
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void beginScrub();
    void scrubToFrame(int64_t frame);
    void endScrub(int64_t frame);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();
    void setHardwareDecode(bool enabled);
    // Configure while closed/paused. Disabled output still prepares real mixed
    // PCM, but uses the presentation clock and never queues device audio.
    void setAudioOutputEnabled(bool enabled);
    // Optional externally advanced monotonic clock for deterministic consumers.
    // No timer, callback or extra thread is created. Configure before open().
    void setManualClock(double monotonicSeconds);
    Snapshot snapshot() const;
    bool isCurrentGeneration(uint64_t generation) const;

    // Render-thread-only: ALL decoded callbacks, including first/seek/scrub,
    // originate here. A negative wait stops the render clock until onWakeRender.
    Pump pumpFrame(int& waitMs);
    std::function<void(DecodedFrame)> onFrameDecoded;
    // Decode-worker observer/sink of the actual post-mix PCM, including silence.
    // Install before opening; do not mutate callbacks while decoding. The sink
    // must not block and must reject old generations after seek/close.
    std::function<void(std::shared_ptr<const AudioFrame>, uint64_t)> onAudioPrepared;
    std::function<void()> onWakeRender;
    std::function<void(State)> onStateChanged;
    std::function<void(double)> onPositionChanged;
    std::function<void(double)> onDurationChanged;
    std::function<void()> onEndOfStream;
    std::function<void(const std::string&)> onOpenFailed;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void requestFrame(int64_t frame, bool scrub, bool resume);
};

} // namespace heisenberg
