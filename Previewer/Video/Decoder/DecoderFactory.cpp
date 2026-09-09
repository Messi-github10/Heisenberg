//
// Created by NiceFold on 2026/6/30.
//

#include <Video/Decoder/DecoderFactory.hpp>
#include <Video/Decoder/SoftwareDecoder.hpp>
#include <Video/Decoder/D3D11Decoder.hpp>
#include <Video/Decoder/CudaDecoder.hpp>

#include <Platform/D3D11/D3D11Context.hpp>
#include <windows.h>
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <memory>

namespace heisenberg {
namespace decoder {

namespace {

/// 尝试检测 D3D11 是否可用。
bool d3d11Available() {
    auto& context = renderer::D3D11Context::instance();
    if (!context.device() || !context.sharedResourceTier2()) return false;
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return false;
    auto* hwContext = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3d11Context = reinterpret_cast<AVD3D11VADeviceContext*>(hwContext->hwctx);
    if (!d3d11Context) {
        av_buffer_unref(&ref);
        return false;
    }
    // FFmpeg releases AVD3D11VADeviceContext::device when the AVHW device
    // context is destroyed. Retain a reference for that ownership.
    context.device()->AddRef();
    d3d11Context->device = context.device();
    d3d11Context->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    d3d11Context->MiscFlags = 0;
    const bool available = av_hwdevice_ctx_init(ref) >= 0;
    av_buffer_unref(&ref);
    return available;
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
