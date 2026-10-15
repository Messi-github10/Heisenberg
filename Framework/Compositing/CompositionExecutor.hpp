#pragma once

#include <Common/AudioFrame.hpp>
#include <Common/VideoFrame.hpp>
#include <Compositing/CompositionPlan.hpp>
#include <functional>
#include <memory>
#include <utility>

namespace heisenberg {

// Native source timestamps are never replaced with presentation timestamps.
struct CompositionVideoSample {
    std::shared_ptr<VideoFrame> source;
    TimelineSettings settings;
    int64_t targetPosition = 0;
    Rational targetTimeBase{1, 1};
    uint64_t generation = 0;
    bool sourceMode = false;
    bool blank = false;
    std::string clipId;
    std::string mediaRef;
    int64_t clipLocalFrame = 0;
    // Copy-owned immutable snapshot, safe to pass to a render thread.
    std::shared_ptr<const CompositionPlan> plan;
};

enum class CompositionStatus { Ok, Cancelled, End, Failure };
enum class CompositionErrorPolicy { Strict, Preview };

template<class T> struct CompositionResult {
    CompositionStatus status = CompositionStatus::Ok;
    T value{};
    std::string error;
    explicit operator bool() const { return status == CompositionStatus::Ok; }
};

// Synchronous, instance-local decode/mix resources only: no worker, renderer,
// window, clock, audio device, dispatcher or asynchronously retained callback.
// Serialize calls on each instance; independent instances may run concurrently.
class CompositionExecutor {
public:
    using Cancel = std::function<bool()>;
    struct Context {
        uint64_t generation;
        Cancel cancelled;
        Context(uint64_t value = 0, Cancel cancel = {})
            : generation(value), cancelled(std::move(cancel)) {}
    };
    struct Options {
        bool hardwareDecode = false;
        CompositionErrorPolicy errorPolicy = CompositionErrorPolicy::Strict;
    };
    struct Info {
        TimelineSettings settings;
        int64_t frameCount = 0;
        int64_t audioSampleCount = 0;
        double duration = 0.0;
        bool sourceMode = false;
        bool seekable = false;
        bool hasVideo = false;
        bool hasAudio = false;
    };
    using OpenResult = CompositionResult<Info>;
    using VideoResult = CompositionResult<CompositionVideoSample>;
    using AudioResult = CompositionResult<std::shared_ptr<AudioFrame>>;

    CompositionExecutor();
    ~CompositionExecutor();
    CompositionExecutor(const CompositionExecutor&) = delete;
    CompositionExecutor& operator=(const CompositionExecutor&) = delete;

    OpenResult open(const CompositionPlan& plan, Options options, Context context = {});
    OpenResult openSource(const std::string& path, Options options, Context context = {});
    VideoResult videoFrame(int64_t frame, Context context = {});
    // Absolute output sample indices, half-open [begin, end). Result position is
    // begin. Clamps end to audioSampleCount; End means begin is past the end.
    // Chunk boundaries do not reset source PTS, interpolation or fade envelopes.
    AudioResult audioSamples(int64_t begin, int64_t end, Context context = {});
    void close();
    // Clears decode history for generation-changing preview seeks, not the plan.
    void reset();
    const Info& info() const;
    const CompositionPlan& plan() const;
    const std::vector<std::string>& warnings() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
