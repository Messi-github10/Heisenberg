//
// Created by NiceFold on 2026/6/30.
//

#include <Decoder/DecoderFactory.hpp>
#include <Decoder/SoftwareDecoder.hpp>
#include <Decoder/D3D11Decoder.hpp>
#include <Decoder/CudaDecoder.hpp>

#include <memory>

namespace heisenberg {
namespace decoder {

namespace {

/// 尝试检测 D3D11 是否可用。
bool d3d11Available() {
    // TODO: 通过 FFmpeg av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_D3D11VA) 探测
    return false;
}

/// 尝试检测 CUDA 是否可用。
bool cudaAvailable() {
    // TODO: 通过 FFmpeg av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA) 探测
    return false;
}

} // namespace

std::unique_ptr<IDecoder> createDecoder(const DecoderConfig& config) {

    // 1. 尝试首选后端
    switch (config.preferred) {
    case DecoderBackend::D3D11:
        if (d3d11Available()) {
            return std::make_unique<D3D11Decoder>();
        }
        break;

    case DecoderBackend::CUDA:
        if (cudaAvailable()) {
            return std::make_unique<CudaDecoder>();
        }
        break;

    case DecoderBackend::Software:
        return std::make_unique<SoftwareDecoder>();
    }

    // 2. 首选不可用，且允许回退 → Software
    if (config.allowFallback) {
        return std::make_unique<SoftwareDecoder>();
    }

    // 3. 不允回退 → 失败
    return nullptr;
}

} // namespace decoder
} // namespace heisenberg
