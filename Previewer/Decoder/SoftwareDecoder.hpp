//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <Decoder/IDecoder.hpp>

namespace heisenberg {
namespace decoder {

class SoftwareDecoder final : public IDecoder {
public:
    SoftwareDecoder();
    ~SoftwareDecoder() override;

    int open(const Stream& stream) override;
    void close() override;
    bool isOpen() const override;

    int sendPacket(std::shared_ptr<const Packet> packet) override;
    std::shared_ptr<AVFrame> receiveFrame() override;

    void flush() override;

    DecoderBackend backend() const override;
    bool isHardware() const override;
    int outputPixelFormat() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace decoder
} // namespace heisenberg
