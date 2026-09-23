#include "ProducerPump.hpp"

#include <Common/AudioFrame.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace {

void avframeDeleter(AVFrame* frame) {
    av_frame_free(&frame);
}

} // namespace

ProducerPump::ProducerPump(RingBuffer<MediaFrame>& videoBuffer,
                           RingBuffer<MediaFrame>& audioBuffer)
    : buffer_(&videoBuffer)
    , audioBuffer_(&audioBuffer) {}

ProducerPump::~ProducerPump() {
    stop();
}

void ProducerPump::start(IProducer* producer) {
    stop();
    producer_ = producer;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Open;
    }
    thread_ = std::thread(&ProducerPump::runLoop, this);
    cmdCv_.notify_all();
}

void ProducerPump::stop() {
    if (!running_ && !thread_.joinable()) {
        producer_ = nullptr;
        return;
    }

    running_ = false;
    scrubbing_ = false;
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::None;
    }
    cmdCv_.notify_all();
    buffer_->abort();
    audioBuffer_->abort();
    if (thread_.joinable()) thread_.join();
    producer_ = nullptr;
    lastVideo_.reset();
    lastPumpedFrame_ = -1;
}

void ProducerPump::seek(int64_t frameIndex, int64_t currentFrameIndex) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Seek;
        seekTarget_ = frameIndex;
        seekOrigin_ = currentFrameIndex;
        buffer_->interrupt();
        audioBuffer_->interrupt();
    }
    cmdCv_.notify_all();
}

void ProducerPump::beginScrub() {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        scrubbing_.store(true, std::memory_order_release);
        buffer_->interrupt();
        audioBuffer_->interrupt();
    }
    cmdCv_.notify_all();
}

void ProducerPump::scrubToFrame(int64_t targetFrame, int64_t currentFrame,
                                uint64_t requestId, bool resumePrefetch) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Scrub;
        scrubTargetFrame_ = targetFrame;
        scrubOriginFrame_ = currentFrame;
        scrubRequestId_ = requestId;
        resumePrefetchAfterScrub_ = resumePrefetch;
        buffer_->interrupt();
        audioBuffer_->interrupt();
    }
    cmdCv_.notify_all();
}

ProducerPump::Cmd ProducerPump::dequeueCommand() {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    const Cmd cmd = pendingCmd_;
    pendingCmd_ = Cmd::None;
    return cmd;
}

ProducerPump::Cmd ProducerPump::waitForCommand() {
    std::unique_lock<std::mutex> lock(cmdMutex_);
    cmdCv_.wait(lock, [this] {
        return pendingCmd_ != Cmd::None || !running_;
    });
    if (!running_) return Cmd::None;
    const Cmd cmd = pendingCmd_;
    pendingCmd_ = Cmd::None;
    return cmd;
}

ProducerFrame ProducerPump::pull(int64_t position) {
    if (!producer_) return {};
    return producer_->getFrame(position);
}

ProducerPump::FramePtr ProducerPump::stampVideo(const ProducerFrame& frame) const {
    if (!producer_ || !frame.video) return {};
    AVFrame* clone = av_frame_clone(frame.video.get());
    if (!clone) return {};
    const Fraction timeBase = producer_->profile().timeBase();
    clone->pts = frame.position;
    clone->time_base = {timeBase.num, timeBase.den};
    clone->duration = 1;
    return FramePtr(clone, avframeDeleter);
}

AudioFramePtr ProducerPump::stampAudio(const ProducerFrame& frame) const {
    if (!producer_) return {};
    AudioFramePtr audio = frame.audio;
    if (!audio) audio = silentAudio(frame.position);
    if (!audio) return {};
    audio->setPosition(producer_->profile().audioSamplePosition(frame.position));
    return audio;
}

AudioFramePtr ProducerPump::silentAudio(int64_t position) const {
    if (!producer_) return {};
    auto audio = std::make_shared<AudioFrame>();
    AudioSpec spec;
    spec.sampleRate = producer_->profile().sampleRate();
    spec.channels = producer_->profile().channels();
    spec.layout = defaultLayout(spec.channels);
    audio->setSpec(spec);
    audio->setSamples(static_cast<int>(
        producer_->profile().audioSamplesForFrame(position)), true);
    audio->clear();
    audio->setPosition(producer_->profile().audioSamplePosition(position));
    return audio;
}

bool ProducerPump::pushVideo(const FramePtr& frame) {
    if (!frame) return false;
    MediaFrame output = MediaFrame::video(frame, generation_);
    output.metadata.pts = frame->pts;
    output.metadata.duration = frame->duration;
    output.metadata.timeBaseNum = frame->time_base.num;
    output.metadata.timeBaseDen = frame->time_base.den;
    return buffer_->push(std::move(output));
}

bool ProducerPump::pushVideoFront(const FramePtr& frame) {
    if (!frame) return false;
    MediaFrame output = MediaFrame::video(frame, generation_);
    output.metadata.pts = frame->pts;
    output.metadata.duration = frame->duration;
    output.metadata.timeBaseNum = frame->time_base.num;
    output.metadata.timeBaseDen = frame->time_base.den;
    return buffer_->pushFront(std::move(output));
}

bool ProducerPump::pushAudio(const AudioFramePtr& frame) {
    if (!frame || !hasAudio_) return true;
    return audioBuffer_->push(MediaFrame::audio(frame, generation_));
}

void ProducerPump::queuePendingEof() {
    if (!pendingEof_ || !running_) return;
    if (!buffer_->isAborted()) {
        buffer_->push(MediaFrame::eof(generation_));
    }
    if (hasAudio_ && !audioBuffer_->isAborted()) {
        audioBuffer_->push(MediaFrame::eof(generation_));
    }
    eof_ = true;
    pendingEof_ = false;
}

void ProducerPump::processCommand(Cmd cmd) {
    switch (cmd) {
    case Cmd::Open: {
        ++generation_;
        eof_ = false;
        pendingEof_ = false;
        scrubbing_ = false;
        lastPumpedFrame_ = -1;
        lastVideo_.reset();
        lastVideoQueued_ = true;
        hasAudio_ = false;
        audioSpec_ = {};
        if (!producer_) {
            if (onOpenFailed) onOpenFailed("Playlist producer is missing");
            break;
        }

        const ProducerFrame first = pull(0);
        lastVideo_ = stampVideo(first);
        if (!lastVideo_) {
            if (onOpenFailed) onOpenFailed("Failed to decode the first timeline frame");
            break;
        }

        hasAudio_ = static_cast<bool>(first.audio);
        audioSpec_.sampleRate = producer_->profile().sampleRate();
        audioSpec_.channels = producer_->profile().channels();
        audioSpec_.layout = defaultLayout(audioSpec_.channels);
        lastPumpedFrame_ = 0;

        buffer_->resume();
        audioBuffer_->resume();
        if (pushVideo(lastVideo_)) lastVideoQueued_ = true;
        if (hasAudio_) pushAudio(stampAudio(first));
        if (producer_->length() <= 1) pendingEof_ = true;
        queuePendingEof();

        const double fps = producer_->profile().fps();
        const double duration = producer_->profile().secondsFromFrame(
            producer_->length());
        if (onOpened) {
            onOpened(duration, fps, producer_->seekable(), lastVideo_,
                     generation_, audioSpec_, hasAudio_);
        }
        break;
    }

    case Cmd::Close:
        ++generation_;
        eof_ = true;
        pendingEof_ = false;
        lastPumpedFrame_ = -1;
        lastVideo_.reset();
        buffer_->flush();
        buffer_->resume();
        audioBuffer_->flush();
        audioBuffer_->resume();
        break;

    case Cmd::Seek: {
        if (!producer_) break;
        ++generation_;
        int64_t target = 0;
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ != Cmd::None) break;
            target = seekTarget_;
        }
        target = std::clamp<int64_t>(
            target, 0, std::max<int64_t>(0, producer_->length() - 1));
        eof_ = false;
        pendingEof_ = false;
        buffer_->resume();
        audioBuffer_->resume();
        audioBuffer_->flush();

        const ProducerFrame frame = pull(target);
        lastVideo_ = stampVideo(frame);
        lastPumpedFrame_ = target;
        lastVideoQueued_ = false;
        if (lastVideo_) {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ == Cmd::None) {
                buffer_->resume();
                if (pushVideoFront(lastVideo_)) lastVideoQueued_ = true;
                if (hasAudio_) pushAudio(stampAudio(frame));
                if (target >= producer_->length() - 1) pendingEof_ = true;
                queuePendingEof();
                const double fps = producer_->profile().fps();
                const double duration = producer_->profile().secondsFromFrame(
                    producer_->length());
                if (onOpened) {
                    onOpened(duration, fps, producer_->seekable(), lastVideo_,
                             generation_, audioSpec_, hasAudio_);
                }
            }
        }
        break;
    }

    case Cmd::Scrub: {
        if (!producer_) break;
        int64_t target = 0;
        uint64_t requestId = 0;
        bool resumePrefetch = false;
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ != Cmd::None) break;
            target = scrubTargetFrame_;
            requestId = scrubRequestId_;
            resumePrefetch = resumePrefetchAfterScrub_;
        }
        target = std::clamp<int64_t>(
            target, 0, std::max<int64_t>(0, producer_->length() - 1));
        eof_ = false;
        pendingEof_ = false;
        buffer_->resume();
        audioBuffer_->resume();

        const ProducerFrame frame = pull(target);
        FramePtr video = stampVideo(frame);
        if (video) {
            lastVideo_ = video;
            lastPumpedFrame_ = target;
            lastVideoQueued_ = false;
        }

        std::lock_guard<std::mutex> lock(cmdMutex_);
        if (pendingCmd_ != Cmd::None || requestId != scrubRequestId_) break;
        if (resumePrefetch) {
            buffer_->resume();
            audioBuffer_->resume();
            if (video) {
                if (!pushVideoFront(video)) break;
                lastVideoQueued_ = true;
                if (hasAudio_) pushAudio(stampAudio(frame));
                if (target >= producer_->length() - 1) pendingEof_ = true;
                queuePendingEof();
            }
            scrubbing_.store(false, std::memory_order_release);
        }
        if ((video || resumePrefetch) && onScrubFrame) {
            onScrubFrame(requestId, video);
        }
        break;
    }

    default:
        break;
    }
}

void ProducerPump::runLoop() {
    while (running_) {
        Cmd cmd = dequeueCommand();
        if (cmd != Cmd::None) {
            processCommand(cmd);
            continue;
        }
        if (!producer_) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }
        if (scrubbing_.load(std::memory_order_acquire) || eof_) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }
        if (lastVideo_ && !lastVideoQueued_) {
            if (pushVideo(lastVideo_)) lastVideoQueued_ = true;
            continue;
        }

        const int64_t next = lastPumpedFrame_ + 1;
        if (next >= producer_->length()) {
            pendingEof_ = true;
            queuePendingEof();
            continue;
        }

        const ProducerFrame frame = pull(next);
        FramePtr video = stampVideo(frame);
        if (!video) {
            pendingEof_ = true;
            queuePendingEof();
            continue;
        }
        lastVideo_ = video;
        lastPumpedFrame_ = next;
        lastVideoQueued_ = false;
        if (pushVideo(video)) lastVideoQueued_ = true;
        if (hasAudio_) pushAudio(stampAudio(frame));
        if (next >= producer_->length() - 1) {
            pendingEof_ = true;
            queuePendingEof();
        }
    }
}

} // namespace heisenberg
