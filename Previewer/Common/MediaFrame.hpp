#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

struct AVFrame;

namespace heisenberg {

class AudioFrame;
class Packet;

// A media item travelling through the demux/decode/filter pipeline.
// The payload is type-safe; shared ownership supports queues and seek caches.
enum class MediaFrameType {
    None,
    Packet,
    Video,
    Audio,
    Eof,
};

struct FrameMetadata {
    int64_t pts = 0;
    int64_t duration = 0;
    int timeBaseNum = 0;
    int timeBaseDen = 1;
    int streamIndex = -1;
    uint64_t generation = 0;
};

using PacketPtr = std::shared_ptr<const Packet>;
using VideoFramePtr = std::shared_ptr<AVFrame>;
using AudioFramePtr = std::shared_ptr<AudioFrame>;
using MediaPayload = std::variant<std::monostate, PacketPtr, VideoFramePtr,
                                  AudioFramePtr>;

struct MediaFrame {
    MediaFrameType type = MediaFrameType::None;
    MediaPayload payload;
    FrameMetadata metadata;

    static MediaFrame video(VideoFramePtr frame, uint64_t generation = 0) {
        MediaFrame result;
        result.type = MediaFrameType::Video;
        result.payload = std::move(frame);
        result.metadata.generation = generation;
        return result;
    }

    static MediaFrame audio(AudioFramePtr frame, uint64_t generation = 0) {
        MediaFrame result;
        result.type = MediaFrameType::Audio;
        result.payload = std::move(frame);
        result.metadata.generation = generation;
        return result;
    }

    static MediaFrame packet(PacketPtr packet, uint64_t generation = 0) {
        MediaFrame result;
        result.type = MediaFrameType::Packet;
        result.payload = std::move(packet);
        result.metadata.generation = generation;
        return result;
    }

    static MediaFrame eof(uint64_t generation = 0) {
        MediaFrame result;
        result.type = MediaFrameType::Eof;
        result.metadata.generation = generation;
        return result;
    }

    bool isData() const {
        return type == MediaFrameType::Packet ||
               type == MediaFrameType::Video ||
               type == MediaFrameType::Audio;
    }

    bool isSignal() const { return type == MediaFrameType::Eof; }

    VideoFramePtr videoFrame() const {
        if (type != MediaFrameType::Video) return {};
        return std::get<VideoFramePtr>(payload);
    }

    AudioFramePtr audioFrame() const {
        if (type != MediaFrameType::Audio) return {};
        return std::get<AudioFramePtr>(payload);
    }
};

} // namespace heisenberg
