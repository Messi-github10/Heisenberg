#include "DecodeThread.hpp"
#include <Decoder/DecoderFactory.hpp>
#include <Decoder/SoftwareDecoder.hpp>
#include <Demuxer/DemuxerFactory.hpp>
#include <Common/Packet.hpp>
#include <Common/FrameTime.hpp>
#include <Common/Stream.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {

namespace {
constexpr double kSeekPrerollSeconds = 2.0;
constexpr int64_t kForwardDecodeThresholdFrames = 64;
constexpr std::size_t kScrubFrameCacheCapacity = 16;

} // namespace

DecodeThread::DecodeThread(RingBuffer<FramePtr>& buffer)
    : buffer_(&buffer) {}

DecodeThread::~DecodeThread() {
    stop();
}

void DecodeThread::start() {
    if (running_) return;
    running_ = true;
    thread_  = std::thread(&DecodeThread::runLoop, this);
}

void DecodeThread::stop() {
    if (!running_) return;

    running_ = false;
    scrubbing_ = false;

    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::None;
    }
    cmdCv_.notify_all();
    buffer_->abort();

    if (thread_.joinable()) {
        thread_.join();
    }

    demuxer_.reset();
    decoder_.reset();
    videoStream_ = nullptr;
    clearScrubFrameCache();
}

void DecodeThread::open(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Open;
        openPath_   = path;
        buffer_->abort();
    }
    cmdCv_.notify_all();
}

void DecodeThread::close() {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Close;
        buffer_->abort();
    }
    cmdCv_.notify_all();
}

void DecodeThread::seek(double seconds, double currentSeconds) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Seek;
        seekTarget_ = seconds;
        seekOrigin_ = currentSeconds;
        buffer_->interrupt();
    }
    cmdCv_.notify_all();
}

void DecodeThread::beginScrub() {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        scrubbing_.store(true, std::memory_order_release);
        scrubDecoderDetached_ = false;
        buffer_->interrupt();
    }
    cmdCv_.notify_all();
}

void DecodeThread::scrubToFrame(int64_t targetFrame, int64_t currentFrame,
                                uint64_t requestId, bool resumePrefetch) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Scrub;
        scrubTargetFrame_ = targetFrame;
        scrubOriginFrame_ = currentFrame;
        scrubRequestId_ = requestId;
        resumePrefetchAfterScrub_ = resumePrefetch;
        buffer_->interrupt();
    }
    cmdCv_.notify_all();
}

DecodeThread::Cmd DecodeThread::dequeueCommand() {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    Cmd cmd = pendingCmd_;
    pendingCmd_ = Cmd::None;
    return cmd;
}

DecodeThread::Cmd DecodeThread::waitForCommand() {
    std::unique_lock<std::mutex> lock(cmdMutex_);
    cmdCv_.wait(lock, [this] {
        return pendingCmd_ != Cmd::None || !running_;
    });
    if (!running_) return Cmd::None;
    Cmd cmd = pendingCmd_;
    pendingCmd_ = Cmd::None;
    return cmd;
}

void DecodeThread::processCommand(Cmd cmd) {
    switch (cmd) {

    case Cmd::Open: {
        if (demuxer_) { demuxer_->close(); demuxer_.reset(); }
        if (decoder_) { decoder_->close(); decoder_.reset(); }
        videoStream_  = nullptr;
        durationSecs_ = 0.0;
        fps_          = 0.0;
        eof_          = false;
        scrubbing_    = false;
        scrubDecoderDetached_ = false;
        resetDecodePosition();
        clearScrubFrameCache();

        demuxer_ = demuxer::createDemuxer();
        if (demuxer_->open(openPath_) < 0) {
            LOG_ERROR("DecodeThread: failed to open — {}", openPath_);
            demuxer_.reset();
            if (onOpenFailed) onOpenFailed("Failed to open file: " + openPath_);
            break;
        }

        durationSecs_ = demuxer_->duration();

        for (const auto& s : demuxer_->streams()) {
            if (s.isVideo()) {
                videoStream_ = &s;
                break;
            }
        }
        if (!videoStream_) {
            LOG_ERROR("DecodeThread: no video stream in {}", openPath_);
            demuxer_->close(); demuxer_.reset();
            if (onOpenFailed) onOpenFailed("No video stream found");
            break;
        }

        fps_ = videoStream_->codec.fps();

        decoder::DecoderConfig cfg;
        cfg.preferred     = decoder::DecoderBackend::Software;
        cfg.allowFallback = false;

        decoder_ = decoder::createDecoder(cfg);
        if (!decoder_ || decoder_->open(*videoStream_) < 0) {
            LOG_ERROR("DecodeThread: failed to create decoder");
            demuxer_->close(); demuxer_.reset();
            decoder_.reset();
            videoStream_ = nullptr;
            if (onOpenFailed) onOpenFailed("Failed to create decoder");
            break;
        }

        auto firstFrame = decodeFrameAt(0.0);

        buffer_->resume();

        if (firstFrame) {
            if (buffer_->push(firstFrame)
                && firstFrame == lastDecodedFrame_) {
                lastDecodedFrameQueued_ = true;
            }

            LOG_INFO("DecodeThread: opened {} — {:.2f}s, {:.2f} fps",
                     openPath_, durationSecs_, fps_);
            bool seekable = demuxer_->seekable();
            if (onOpened) onOpened(durationSecs_, fps_, seekable, firstFrame);
        } else {
            LOG_ERROR("DecodeThread: failed to decode first frame");
            if (onOpenFailed) onOpenFailed("Failed to decode first frame");
        }
        break;
    }

    case Cmd::Close:
        if (decoder_) { decoder_->close(); decoder_.reset(); }
        if (demuxer_) { demuxer_->close(); demuxer_.reset(); }
        videoStream_  = nullptr;
        durationSecs_ = 0.0;
        fps_          = 0.0;
        eof_          = false;
        scrubbing_    = false;
        scrubDecoderDetached_ = false;
        resetDecodePosition();
        clearScrubFrameCache();
        buffer_->flush();
        buffer_->resume();
        break;

    case Cmd::Seek: {
        if (!demuxer_ || !decoder_ || !videoStream_) break;

        double targetSeconds;
        double currentSeconds;
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            // A newer command supersedes this one before it starts doing work.
            if (pendingCmd_ != Cmd::None) break;
            targetSeconds = seekTarget_;
            currentSeconds = seekOrigin_;
        }

        eof_ = false;
        buffer_->resume();

        auto targetFrame = decodeFrameAt(targetSeconds, currentSeconds);

        if (targetFrame) {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ == Cmd::None) {
                buffer_->resume();
                if (buffer_->pushFront(targetFrame)) {
                    if (targetFrame == lastDecodedFrame_) {
                        lastDecodedFrameQueued_ = true;
                    }
                    LOG_DEBUG("DecodeThread: seek to {:.3f}s selected frame at {:.3f}s",
                              targetSeconds, frameTimeSeconds(*targetFrame));
                    bool seekable = demuxer_->seekable();
                    if (onOpened) {
                        onOpened(durationSecs_, fps_, seekable, targetFrame);
                    }
                }
            }
        }
        break;
    }

    case Cmd::Scrub: {
        if (!demuxer_ || !decoder_ || !videoStream_) break;

        int64_t targetFrame;
        int64_t currentFrame;
        uint64_t requestId;
        bool resumePrefetch;
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ != Cmd::None) break;
            targetFrame = scrubTargetFrame_;
            currentFrame = scrubOriginFrame_;
            requestId = scrubRequestId_;
            resumePrefetch = resumePrefetchAfterScrub_;
        }

        eof_ = false;
        buffer_->resume();

        FramePtr frame;
        bool cacheHit = false;

        // A cached preview can be shown immediately while dragging. The final
        // request still decodes so playback resumes from the correct position.
        if (!resumePrefetch) {
            frame = findCachedScrubFrame(targetFrame);
            cacheHit = static_cast<bool>(frame);
        }

        if (!frame) {
            const int64_t decodeOrigin = scrubDecoderDetached_
                ? -1
                : currentFrame;
            frame = decodeFrameAtFrame(targetFrame, decodeOrigin);
        }

        std::lock_guard<std::mutex> lock(cmdMutex_);
        if (pendingCmd_ != Cmd::None || requestId != scrubRequestId_) break;

        if (frame) {
            if (cacheHit) {
                // The displayed position changed without moving the decoder.
                scrubDecoderDetached_ = true;
                LOG_DEBUG("DecodeThread: LRU hit for scrub frame {}",
                          targetFrame);
            } else {
                scrubDecoderDetached_ = false;
                cacheScrubFrame(targetFrame, frame);
            }
        }

        if (resumePrefetch) {
            buffer_->resume();
            if (frame) {
                if (!buffer_->pushFront(frame)) break;
                if (frame == lastDecodedFrame_) {
                    lastDecodedFrameQueued_ = true;
                }
            }
            scrubbing_.store(false, std::memory_order_release);
        }

        // A null final result still lets the controller leave Scrubbing.
        if ((frame || resumePrefetch) && onScrubFrame) {
            onScrubFrame(requestId, frame);
        }
        break;
    }

    default:
        break;
    }
}

DecodeThread::FramePtr DecodeThread::decodeFrameAt(double targetSeconds,
                                                    double currentSeconds) {
    if (!demuxer_ || !decoder_ || !videoStream_) return nullptr;

    AVRational sourceFrameRate = {
        videoStream_->codec.fpsNum,
        videoStream_->codec.fpsDen
    };
    if (sourceFrameRate.num <= 0 || sourceFrameRate.den <= 0) {
        sourceFrameRate = {25, 1};
    }
    const double frameRate = av_q2d(sourceFrameRate);
    targetSeconds = std::max(0.0, targetSeconds);

    // Like MLT, express the seek target on the source video's frame grid.
    int64_t targetFrameIndex = static_cast<int64_t>(
        std::llround(targetSeconds * frameRate));
    if (durationSecs_ > 0.0) {
        const int64_t lastFrameIndex = std::max<int64_t>(
            0, static_cast<int64_t>(std::ceil(durationSecs_ * frameRate)) - 1);
        targetFrameIndex = std::min(targetFrameIndex, lastFrameIndex);
    }

    const int64_t currentFrameIndex = currentSeconds >= 0.0
        ? static_cast<int64_t>(std::llround(currentSeconds * frameRate))
        : -1;

    return decodeFrameAtFrame(targetFrameIndex, currentFrameIndex);
}

DecodeThread::FramePtr DecodeThread::decodeFrameAtFrame(
    int64_t targetFrameIndex, int64_t currentFrameIndex) {
    if (!demuxer_ || !decoder_ || !videoStream_) return nullptr;

    AVRational sourceFrameRate = {
        videoStream_->codec.fpsNum,
        videoStream_->codec.fpsDen
    };
    if (sourceFrameRate.num <= 0 || sourceFrameRate.den <= 0) {
        sourceFrameRate = {25, 1};
    }
    const double frameRate = av_q2d(sourceFrameRate);

    targetFrameIndex = std::max<int64_t>(0, targetFrameIndex);
    if (durationSecs_ > 0.0) {
        const int64_t lastFrameIndex = std::max<int64_t>(
            0, static_cast<int64_t>(std::ceil(durationSecs_ * frameRate)) - 1);
        targetFrameIndex = std::min(targetFrameIndex, lastFrameIndex);
    }

    const double snappedTargetSeconds = targetFrameIndex / frameRate;
    const int64_t forwardDistance = targetFrameIndex - currentFrameIndex;
    bool decodeForward = currentFrameIndex >= 0
        && forwardDistance >= 0
        && forwardDistance <= kForwardDecodeThresholdFrames;

    if (decodeForward) {
        // The producer can be ahead of the displayed frame due to buffering.
        // Reuse buffered frames first, then continue from the decoder cursor.
        while (auto front = buffer_->peekFront()) {
            if (!*front || !hasFrameTimestamp(**front)) {
                decodeForward = false;
                break;
            }

            const int64_t bufferedIndex = frameIndexFromTimestamp(
                **front, sourceFrameRate);
            if (bufferedIndex > targetFrameIndex) {
                decodeForward = false;
                break;
            }

            FramePtr bufferedFrame;
            if (!buffer_->popWithTimeout(bufferedFrame,
                                         std::chrono::milliseconds(0))) {
                return nullptr;
            }
            if (bufferedIndex == targetFrameIndex) {
                LOG_DEBUG("DecodeThread: reused buffered frame {} for seek",
                          targetFrameIndex);
                return bufferedFrame;
            }
        }

        if (decodeForward && lastDecodedFrameIndex_ > targetFrameIndex) {
            decodeForward = false;
        } else if (decodeForward
                   && lastDecodedFrameIndex_ == targetFrameIndex
                   && lastDecodedFrame_) {
            LOG_DEBUG("DecodeThread: reused decoded frame {} for seek",
                      targetFrameIndex);
            return lastDecodedFrame_;
        }
    }

    if (decodeForward) {
        LOG_DEBUG("DecodeThread: decoding forward {} frames without seek",
                  forwardDistance);
    } else {
        buffer_->flush();

        // Decode some preroll for inter-frame codecs and reordered B-frames.
        const double seekSeconds = std::max(
            0.0, snappedTargetSeconds - kSeekPrerollSeconds);
        int seekResult = demuxer_->seek(
            seekSeconds, videoStream_->index, 1 /* AVSEEK_FLAG_BACKWARD */);
        if (seekResult < 0) {
            LOG_ERROR("DecodeThread: seek to {:.3f}s failed with {}",
                      seekSeconds, seekResult);
            return nullptr;
        }
        decoder_->flush();
        resetDecodePosition();
    }

    FramePtr lastFrame = decodeForward ? lastDecodedFrame_ : nullptr;

    auto selectFrame = [&](FramePtr frame) -> FramePtr {
        if (!frame) return nullptr;

        lastFrame = frame;
        if (!hasFrameTimestamp(*frame)) {
            if (targetFrameIndex == 0) {
                frame->pts = 0;
                return frame;
            }
            return nullptr;
        }

        const int64_t decodedFrameIndex = frameIndexFromTimestamp(
            *frame, sourceFrameRate);
        if (decodedFrameIndex >= targetFrameIndex) {
            return frame;
        }
        return nullptr;
    };

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ != Cmd::None) {
                return nullptr;
            }
        }

        auto frame = receiveDecodedFrame();
        if (frame) {
            if (auto selected = selectFrame(std::move(frame))) {
                return selected;
            }
            continue;
        }

        auto pkt = demuxer_->readPacket();
        if (pkt) {
            if (pkt->streamIndex == videoStream_->index) {
                decoder_->sendPacket(pkt);
            }
        } else {
            decoder_->sendPacket(nullptr);

            while (running_) {
                auto drainFrame = receiveDecodedFrame();
                if (!drainFrame) break;
                if (auto selected = selectFrame(std::move(drainFrame))) {
                    return selected;
                }
            }
            if (lastFrame && !hasFrameTimestamp(*lastFrame)) {
                LOG_WARN("DecodeThread: no usable frame timestamp; seek is approximate");
                lastFrame->pts = timestampFromSeconds(snappedTargetSeconds,
                                                      lastFrame->time_base);
            }
            return lastFrame;
        }
    }

    return nullptr;
}

void DecodeThread::runLoop() {
    while (running_) {
        Cmd cmd = dequeueCommand();
        if (cmd != Cmd::None) {
            processCommand(cmd);
            continue;
        }

        if (!demuxer_ || !decoder_) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }

        if (scrubbing_.load(std::memory_order_acquire)) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }

        if (eof_) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }

        // A seek can interrupt a producer blocked on a full buffer after the
        // decoder has already output the frame. Queue it before decoding more.
        if (lastDecodedFrame_ && !lastDecodedFrameQueued_) {
            if (buffer_->push(lastDecodedFrame_)) {
                lastDecodedFrameQueued_ = true;
            }
            continue;
        }

        auto frame = receiveDecodedFrame();
        if (frame) {
            bool ok = buffer_->push(frame);
            if (ok && frame == lastDecodedFrame_) {
                lastDecodedFrameQueued_ = true;
            }
            continue;
        }

        auto pkt = demuxer_->readPacket();
        if (pkt) {
            if (pkt->streamIndex == videoStream_->index) {
                decoder_->sendPacket(pkt);
            }
        } else {
            decoder_->sendPacket(nullptr);

            while (running_) {
                auto drainFrame = receiveDecodedFrame();
                if (drainFrame) {
                    if (!buffer_->push(drainFrame)) break;
                    if (drainFrame == lastDecodedFrame_) {
                        lastDecodedFrameQueued_ = true;
                    }
                } else {
                    break;
                }
            }

            if (!buffer_->isAborted() && running_) {
                LOG_INFO("DecodeThread: end of stream");
                if (onEndOfStream) onEndOfStream();
            }

            eof_ = true;
        }
    }

    if (decoder_) { decoder_->close(); decoder_.reset(); }
    if (demuxer_) { demuxer_->close(); demuxer_.reset(); }
    videoStream_  = nullptr;
    durationSecs_ = 0.0;
    fps_          = 0.0;
    scrubbing_    = false;
    scrubDecoderDetached_ = false;
    resetDecodePosition();
    clearScrubFrameCache();
}

DecodeThread::FramePtr DecodeThread::receiveDecodedFrame() {
    if (!decoder_) return nullptr;

    auto frame = decoder_->receiveFrame();
    if (!frame) return nullptr;

    lastDecodedFrame_ = frame;
    lastDecodedFrameQueued_ = false;
    if (!hasFrameTimestamp(*frame)) {
        lastDecodedFrameIndex_ = -1;
    } else {
        AVRational sourceFrameRate = {
            videoStream_->codec.fpsNum,
            videoStream_->codec.fpsDen
        };
        if (sourceFrameRate.num <= 0 || sourceFrameRate.den <= 0) {
            sourceFrameRate = {25, 1};
        }
        lastDecodedFrameIndex_ = frameIndexFromTimestamp(
            *frame, sourceFrameRate);
    }
    return frame;
}

void DecodeThread::resetDecodePosition() {
    lastDecodedFrameIndex_ = -1;
    lastDecodedFrame_.reset();
    lastDecodedFrameQueued_ = true;
}

DecodeThread::FramePtr DecodeThread::findCachedScrubFrame(
    int64_t frameIndex) {
    auto it = std::find_if(
        scrubFrameCache_.begin(), scrubFrameCache_.end(),
        [frameIndex](const ScrubCacheEntry& entry) {
            return entry.frameIndex == frameIndex;
        });
    if (it == scrubFrameCache_.end()) return nullptr;

    FramePtr frame = it->frame;
    scrubFrameCache_.splice(scrubFrameCache_.begin(),
                            scrubFrameCache_, it);
    return frame;
}

void DecodeThread::cacheScrubFrame(int64_t frameIndex,
                                   const FramePtr& frame) {
    if (!frame) return;

    auto it = std::find_if(
        scrubFrameCache_.begin(), scrubFrameCache_.end(),
        [frameIndex](const ScrubCacheEntry& entry) {
            return entry.frameIndex == frameIndex;
        });
    if (it != scrubFrameCache_.end()) {
        it->frame = frame;
        scrubFrameCache_.splice(scrubFrameCache_.begin(),
                                scrubFrameCache_, it);
        return;
    }

    scrubFrameCache_.push_front({frameIndex, frame});
    if (scrubFrameCache_.size() > kScrubFrameCacheCapacity) {
        scrubFrameCache_.pop_back();
    }
}

void DecodeThread::clearScrubFrameCache() {
    scrubFrameCache_.clear();
}

} // namespace heisenberg
