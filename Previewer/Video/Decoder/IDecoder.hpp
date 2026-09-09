//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

struct AVFrame;

namespace heisenberg {

class Packet;
class Stream;

namespace decoder {

enum class DecoderBackend {
    Software,
    D3D11,
    CUDA
};

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual int open(const Stream& stream) = 0;

    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    virtual int sendPacket(std::shared_ptr<const Packet> packet) = 0;

    virtual std::shared_ptr<AVFrame> receiveFrame() = 0;

    virtual void flush() = 0;

    virtual DecoderBackend backend() const = 0;

    virtual bool isHardware() const = 0;

    virtual int outputPixelFormat() const = 0;
};

} // namespace decoder
} // namespace heisenberg
