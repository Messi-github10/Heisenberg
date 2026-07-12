//
// Created by NiceFold on 2026/7/13.
//

#pragma once

#include <Common/RingBuffer.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
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
    std::function<void(const std::string& reason)>                            onOpenFailed;
    std::function<void()>                                                     onEndOfStream;

    explicit DecodeThread(RingBuffer<FramePtr>& buffer);
    ~DecodeThread();

    void start();
    void stop();
    void open(const std::string& path);
    void close();
    void seek(double seconds);

private:
    enum class Cmd : int {
        None = 0, 
        Open, 
        Close, 
        Seek 
    };

    void        runLoop();
    Cmd         dequeueCommand();
    Cmd         waitForCommand();
    void        processCommand(Cmd cmd);
    FramePtr    decodeKeyFrame(double targetPtsMs);

    std::thread thread_;

    RingBuffer<FramePtr>* buffer_ = nullptr;

    std::mutex              cmdMutex_;
    std::condition_variable cmdCv_;
    Cmd                     pendingCmd_ = Cmd::None;
    std::string             openPath_;
    double                  seekTarget_ = 0.0;

    std::unique_ptr<demuxer::IDemuxer> demuxer_;
    std::unique_ptr<decoder::IDecoder> decoder_;
    const Stream*                      videoStream_  = nullptr;
    double                             durationSecs_ = 0.0;
    double                             fps_          = 0.0;
    bool                               eof_          = false;

    std::atomic<bool> running_{false};
};

} // namespace heisenberg
