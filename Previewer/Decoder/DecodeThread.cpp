#include "DecodeThread.hpp"
#include <Decoder/DecoderFactory.hpp>
#include <Decoder/SoftwareDecoder.hpp>
#include <Demuxer/DemuxerFactory.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {

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
}

void DecodeThread::open(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Open;
        openPath_   = path;
    }
    buffer_->abort();
    cmdCv_.notify_all();
}

void DecodeThread::close() {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Close;
    }
    buffer_->abort();
    cmdCv_.notify_all();
}

void DecodeThread::seek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingCmd_ = Cmd::Seek;
        seekTarget_ = seconds;
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

        auto firstFrame = decodeKeyFrame(0.0);

        buffer_->resume();

        if (firstFrame) {
            buffer_->push(firstFrame);

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
        buffer_->flush();
        buffer_->resume();
        break;

    case Cmd::Seek: {
        if (!demuxer_ || !decoder_ || !videoStream_) break;

        decoder_->flush();
        eof_ = false;

        auto keyFrame = decodeKeyFrame(seekTarget_);

        buffer_->resume();

        if (keyFrame) {
            buffer_->push(keyFrame);
            LOG_DEBUG("DecodeThread: seek to {:.3f}s done", seekTarget_);
            bool seekable = demuxer_->seekable();
            if (onOpened) onOpened(durationSecs_, fps_, seekable, keyFrame);
        }
        break;
    }

    default:
        break;
    }
}

DecodeThread::FramePtr DecodeThread::decodeKeyFrame(double targetPtsMs) {
    if (!demuxer_ || !decoder_ || !videoStream_) return nullptr;

    demuxer_->seek(targetPtsMs, videoStream_->index, 1 /* AVSEEK_FLAG_BACKWARD */);

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(cmdMutex_);
            if (pendingCmd_ != Cmd::None) {
                return nullptr;
            }
        }

        auto frame = decoder_->receiveFrame();
        if (frame) {
            return frame;
        }

        auto pkt = demuxer_->readPacket();
        if (pkt) {
            if (pkt->streamIndex == videoStream_->index) {
                decoder_->sendPacket(pkt);
            }
        } else {
            decoder_->sendPacket(nullptr);
            return decoder_->receiveFrame();
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

        if (eof_) {
            cmd = waitForCommand();
            if (cmd != Cmd::None) processCommand(cmd);
            continue;
        }

        auto frame = decoder_->receiveFrame();
        if (frame) {
            bool ok = buffer_->push(frame);
            if (!ok) {
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
                auto drainFrame = decoder_->receiveFrame();
                if (drainFrame) {
                    if (!buffer_->push(drainFrame)) break;
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
}

} // namespace heisenberg
