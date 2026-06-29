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

    // ---- 生命周期 ----
    int open(const Stream& stream) override;
    void close() override;
    bool isOpen() const override;

    // ---- 解码 ----
    int sendPacket(std::shared_ptr<const Packet> packet) override;
    std::shared_ptr<AVFrame> receiveFrame() override;

    // ---- Seek 支持 ----
    void flush() override;

    // ---- 能力查询 ----
    DecoderBackend backend() const override;
    bool isHardware() const override;
    int outputPixelFormat() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace decoder
} // namespace heisenberg
