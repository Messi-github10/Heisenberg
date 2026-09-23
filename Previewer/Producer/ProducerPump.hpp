#pragma once

#include <Common/AudioSpec.hpp>
#include <Common/MediaFrame.hpp>
#include <Common/RingBuffer.hpp>
#include <Producer/IProducer.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AVFrame;

namespace heisenberg {

class ProducerPump {
public:
    using FramePtr = std::shared_ptr<AVFrame>;

    std::function<void(double durationSecs, double fps, bool seekable,
                       FramePtr firstFrame, uint64_t generation,
                       AudioSpec audioSpec, bool hasAudio)> onOpened;
    std::function<void(uint64_t requestId, FramePtr frame)> onScrubFrame;
    std::function<void(const std::string& reason)> onOpenFailed;

    ProducerPump(RingBuffer<MediaFrame>& videoBuffer,
                 RingBuffer<MediaFrame>& audioBuffer);
    ~ProducerPump();

    ProducerPump(const ProducerPump&) = delete;
    ProducerPump& operator=(const ProducerPump&) = delete;

    void start(IProducer* producer);
    void stop();
    void seek(int64_t frameIndex, int64_t currentFrameIndex);
    void beginScrub();
    void scrubToFrame(int64_t targetFrame, int64_t currentFrame,
                      uint64_t requestId, bool resumePrefetch);

private:
    enum class Cmd {
        None,
        Open,
        Close,
        Seek,
        Scrub
    };

    void runLoop();
    Cmd dequeueCommand();
    Cmd waitForCommand();
    void processCommand(Cmd cmd);
    FramePtr stampVideo(const ProducerFrame& frame) const;
    AudioFramePtr stampAudio(const ProducerFrame& frame) const;
    AudioFramePtr silentAudio(int64_t position) const;
    bool pushVideo(const FramePtr& frame);
    bool pushVideoFront(const FramePtr& frame);
    bool pushAudio(const AudioFramePtr& frame);
    void queuePendingEof();
    ProducerFrame pull(int64_t position);

    RingBuffer<MediaFrame>* buffer_ = nullptr;
    RingBuffer<MediaFrame>* audioBuffer_ = nullptr;
    IProducer* producer_ = nullptr;

    std::thread thread_;
    std::mutex cmdMutex_;
    std::condition_variable cmdCv_;
    Cmd pendingCmd_ = Cmd::None;
    int64_t seekTarget_ = 0;
    int64_t seekOrigin_ = -1;
    int64_t scrubTargetFrame_ = 0;
    int64_t scrubOriginFrame_ = -1;
    uint64_t scrubRequestId_ = 0;
    bool resumePrefetchAfterScrub_ = false;

    AudioSpec audioSpec_;
    bool hasAudio_ = false;
    bool eof_ = false;
    bool pendingEof_ = false;
    int64_t lastPumpedFrame_ = -1;
    FramePtr lastVideo_;
    bool lastVideoQueued_ = true;
    uint64_t generation_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> scrubbing_{false};
};

} // namespace heisenberg
