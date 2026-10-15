//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace heisenberg {

class Packet;
class Stream;

namespace demuxer {

enum class ReadStatus { Packet, End, Cancelled, Failure };
struct PacketReadResult {
    ReadStatus status = ReadStatus::Failure;
    std::shared_ptr<Packet> packet;
    int error = 0;
};

class IDemuxer {
public:
    virtual ~IDemuxer() = default;

    virtual int open(const std::string &url) = 0;

    virtual void close() = 0;

    virtual std::shared_ptr<Packet> readPacket() = 0;
    // Legacy implementations remain source-compatible; FFmpeg overrides this
    // to distinguish AVERROR_EOF from an actual read/interrupt failure.
    virtual PacketReadResult readPacketResult() {
        auto packet = readPacket();
        return {packet ? ReadStatus::Packet : ReadStatus::End, std::move(packet), 0};
    }
    virtual void setInterrupt(std::function<bool()> cancelled) { (void)cancelled; }

    // Seeks to a zero-based media time. The implementation converts it to
    // the selected stream's native time base.
    virtual int seek(double seconds, int streamIndex = -1, int flags = 1 /* AVSEEK_FLAG_BACKWARD */) = 0;

    virtual const std::vector<Stream> &streams() const = 0;

    virtual double duration() const = 0;

    virtual bool seekable() const = 0;

    virtual bool isOpen() const = 0;

    virtual const std::string &url() const = 0;

    virtual double startTime() const = 0;

    virtual std::string formatName() const = 0;
};

} // namespace demuxer
} // namespace heisenberg
