//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

class Packet;
class Stream;

namespace demuxer {

class IDemuxer {
public:
    virtual ~IDemuxer() = default;

    virtual int open(const std::string &url) = 0;

    virtual void close() = 0;

    virtual std::shared_ptr<Packet> readPacket() = 0;

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
