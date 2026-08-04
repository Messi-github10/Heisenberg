//
// Created by NiceFold on 2026/7/13.
//

#pragma once

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

namespace demuxer { class IDemuxer; }
namespace decoder { class IDecoder; }
class Stream;

class DecodeThread {
public:
    using FramePtr = std::shared_ptr<AVFrame>;

    std::function<void(double durationSecs, double fps, bool seekable,
                       FramePtr firstFrame)> onOpened;
    std::function<void(uint64_t requestId, FramePtr frame)>                   onScrubFrame;
    std::function<void(const std::string& reason)>                            onOpenFailed;
    std::function<void()>                                                     onEndOfStream;

    explicit DecodeThread(RingBuffer<FramePtr>& buffer);
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
    FramePtr    receiveDecodedFrame();
    void        resetDecodePosition();
    FramePtr    findCachedScrubFrame(int64_t frameIndex);
    void        cacheScrubFrame(int64_t frameIndex, const FramePtr& frame);
    void        clearScrubFrameCache();

    struct ScrubCacheEntry {
        int64_t  frameIndex = 0;
        FramePtr frame;
    };

    std::thread thread_;

    RingBuffer<FramePtr>* buffer_ = nullptr;

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

    std::unique_ptr<demuxer::IDemuxer> demuxer_;
    std::unique_ptr<decoder::IDecoder> decoder_;
    const Stream*                      videoStream_  = nullptr;
    double                             durationSecs_ = 0.0;
    double                             fps_          = 0.0;
    bool                               eof_          = false;
    int64_t                            lastDecodedFrameIndex_ = -1;
    FramePtr                           lastDecodedFrame_;
    bool                               lastDecodedFrameQueued_ = true;
    std::list<ScrubCacheEntry>         scrubFrameCache_;
    bool                               scrubDecoderDetached_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> scrubbing_{false};
};

} // namespace heisenberg
