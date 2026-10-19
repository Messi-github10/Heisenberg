#pragma once

#include <Common/AudioFrame.hpp>
#include <Common/VideoFrame.hpp>
#include <Compositing/CompositionPlan.hpp>
#include <functional>
#include <memory>
#include <utility>

namespace heisenberg {

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
    std::shared_ptr<const CompositionPlan> plan;
};

enum class CompositionStatus { Succeeded, Cancelled, End, Failure };
enum class CompositionErrorPolicy { Strict, Preview };

template<class T> struct CompositionResult {
    CompositionStatus status = CompositionStatus::Succeeded;
    T value{};
    std::string error;
    explicit operator bool() const {
        return status == CompositionStatus::Succeeded;
    }
};

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
    AudioResult audioSamples(int64_t begin, int64_t end, Context context = {});
    void close();
    void reset();
    const Info& info() const;
    const CompositionPlan& plan() const;
    const std::vector<std::string>& warnings() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
