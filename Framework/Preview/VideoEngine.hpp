#pragma once

#include <Common/VideoFrame.hpp>
#include <Common/AudioFrame.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <functional>
#include <memory>

namespace heisenberg {

struct Timeline;
class MediaManifest;
class ProjectSnapshot;
struct ColorSettings;

using DecodedFrame = CompositionVideoSample;

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
    bool openTimeline(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir, const ColorSettings& colorSettings);
    bool openComposition(const CompositionPlan& plan);
    bool openSnapshot(const ProjectSnapshot& snapshot);
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
    void setAudioOutputEnabled(bool enabled);
    void setManualClock(double monotonicSeconds);
    Snapshot snapshot() const;
    bool isCurrentGeneration(uint64_t generation) const;
    Pump pumpFrame(int& waitMs);
    std::function<void(DecodedFrame)> onFrameDecoded;
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
