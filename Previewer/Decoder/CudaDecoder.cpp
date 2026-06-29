//
// Created by NiceFold on 2026/6/30.
//

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <Decoder/CudaDecoder.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>

namespace heisenberg {
namespace decoder {

struct CudaDecoder::Impl {};

CudaDecoder::CudaDecoder() : impl_(std::make_unique<Impl>()) {}
CudaDecoder::~CudaDecoder() = default;

int CudaDecoder::open(const Stream&)            { return -1; }
void CudaDecoder::close()                       {}
bool CudaDecoder::isOpen() const                { return false; }
int CudaDecoder::sendPacket(std::shared_ptr<const Packet>) { return -1; }
std::shared_ptr<AVFrame> CudaDecoder::receiveFrame()       { return nullptr; }
void CudaDecoder::flush()                       {}
DecoderBackend CudaDecoder::backend() const     { return DecoderBackend::CUDA; }
bool CudaDecoder::isHardware() const            { return true; }
int CudaDecoder::outputPixelFormat() const      { return AV_PIX_FMT_NONE; }

} // namespace decoder
} // namespace heisenberg
