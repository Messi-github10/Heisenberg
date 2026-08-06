//
// Created by NiceFold on 2026/7/13.
//

#pragma once

#include <Common/MediaFrame.hpp>
#include <Common/AudioSpec.hpp>
#include <Common/RingBuffer.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AVFrame;

namespace heisenberg {

namespace pipeline {
class DecoderNode;
class DemuxSource;
}
class Stream;

class DecodeThread {
public:
    using FramePtr = std::shared_ptr<AVFrame>;

    std::function<void(double durationSecs, double fps, bool seekable,
                       FramePtr firstFrame, uint64_t generation,
                       AudioSpec audioSpec, bool hasAudio)> onOpened;
    std::function<void(uint64_t requestId, FramePtr frame)>                   onScrubFrame;
    std::function<void(const std::string& reason)>                            onOpenFailed;

    DecodeThread(RingBuffer<MediaFrame>& videoBuffer,
                 RingBuffer<MediaFrame>& audioBuffer);
    ~DecodeThread();

    void start();
    void stop();
    void open(const std::string& path);
    void close();
    void seek(double seconds, double currentSeconds);
    void beginScrub();
    void scrubToFrame(int64_t targetFrame, int64_t currentFrame,
                      uint64_t requestId, bool resumePrefetch);

private:
    enum class Cmd : int {
        None = 0, 
        Open, 
        Close, 
        Seek,
        Scrub
    };

    void        runLoop();
    Cmd         dequeueCommand();
    Cmd         waitForCommand();
    void        processCommand(Cmd cmd);
    FramePtr    decodeFrameAt(double targetSeconds, double currentSeconds = -1.0);
    FramePtr    decodeFrameAtFrame(int64_t targetFrameIndex,
                                   int64_t currentFrameIndex = -1);
    FramePtr    receiveDecodedFrame(MediaFrame* signal = nullptr);
    void        resetDecodePosition();
    FramePtr    findCachedScrubFrame(int64_t frameIndex);
    void        cacheScrubFrame(int64_t frameIndex, const FramePtr& frame);
    void        clearScrubFrameCache();
    bool        pushVideo(const FramePtr& frame);
    bool        pushVideoFront(const FramePtr& frame);
    bool        drainAudioOutput(bool queueOutput);
    void        queuePendingEof();

    struct ScrubCacheEntry {
        int64_t  frameIndex = 0;
        FramePtr frame;
    };

    std::thread thread_;

    RingBuffer<MediaFrame>* buffer_ = nullptr;
    RingBuffer<MediaFrame>* audioBuffer_ = nullptr;

    std::mutex              cmdMutex_;
    std::condition_variable cmdCv_;
    Cmd                     pendingCmd_ = Cmd::None;
    std::string             openPath_;
    double                  seekTarget_ = 0.0;
    double                  seekOrigin_ = -1.0;
    int64_t                 scrubTargetFrame_ = 0;
    int64_t                 scrubOriginFrame_ = -1;
    uint64_t                scrubRequestId_ = 0;
    bool                    resumePrefetchAfterScrub_ = false;

    std::unique_ptr<pipeline::DemuxSource> demuxSource_;
    std::unique_ptr<pipeline::DecoderNode> decoderNode_;
    std::unique_ptr<pipeline::DecoderNode> audioDecoderNode_;
    const Stream*                      videoStream_  = nullptr;
    const Stream*                      audioStream_  = nullptr;
    AudioSpec                          audioSpec_;
    double                             durationSecs_ = 0.0;
    double                             fps_          = 0.0;
    bool                               eof_          = false;
    bool                               pendingEof_   = false;
    int64_t                            lastDecodedFrameIndex_ = -1;
    FramePtr                           lastDecodedFrame_;
    bool                               lastDecodedFrameQueued_ = true;
    std::list<ScrubCacheEntry>         scrubFrameCache_;
    bool                               scrubDecoderDetached_ = false;
    uint64_t                           generation_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> scrubbing_{false};
};

} // namespace heisenberg
