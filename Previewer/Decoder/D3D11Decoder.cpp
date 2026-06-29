//
// Created by NiceFold on 2026/6/30.
//

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <Decoder/D3D11Decoder.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>

namespace heisenberg {
namespace decoder {

struct D3D11Decoder::Impl {};

D3D11Decoder::D3D11Decoder() : impl_(std::make_unique<Impl>()) {}
D3D11Decoder::~D3D11Decoder() = default;

int D3D11Decoder::open(const Stream&)            { return -1; }
void D3D11Decoder::close()                       {}
bool D3D11Decoder::isOpen() const                { return false; }
int D3D11Decoder::sendPacket(std::shared_ptr<const Packet>) { return -1; }
std::shared_ptr<AVFrame> D3D11Decoder::receiveFrame()       { return nullptr; }
void D3D11Decoder::flush()                       {}
DecoderBackend D3D11Decoder::backend() const     { return DecoderBackend::D3D11; }
bool D3D11Decoder::isHardware() const            { return true; }
int D3D11Decoder::outputPixelFormat() const      { return AV_PIX_FMT_NONE; }

} // namespace decoder
} // namespace heisenberg
