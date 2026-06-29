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

// -- 解码后端类型 --

enum class DecoderBackend {
    Software,   ///< FFmpeg 软件解码，输出 CPU AVFrame
    D3D11,      ///< Direct3D 11 硬件解码 (dxva2/d3d11va)，输出 D3D11 纹理
    CUDA        ///< NVIDIA CUDA 硬件解码 (nvdec/cuvid)，输出 CUDA 显存
};

// -- 解码器抽象接口 --

class IDecoder {
public:
    virtual ~IDecoder() = default;

    // ---- 生命周期 ----

    /// 根据流信息初始化解码器。
    /// @return 0 成功，负值失败
    virtual int open(const Stream& stream) = 0;

    /// 关闭解码器并释放资源。可重复调用，幂等。
    virtual void close() = 0;

    /// 是否已成功打开且未关闭。
    virtual bool isOpen() const = 0;

    // ---- 解码 ----

    /// 向解码器发送一帧压缩数据包。
    /// @return 0 成功，负值失败（如解码器内部缓冲区满）
    virtual int sendPacket(std::shared_ptr<const Packet> packet) = 0;

    /// 从解码器取出一帧解码后的数据。
    /// @return 解码后的 AVFrame，若当前无输出返回 nullptr。
    ///         硬件后端返回的 AVFrame 像素数据驻留在 GPU 显存中。
    virtual std::shared_ptr<AVFrame> receiveFrame() = 0;

    // ---- Seek 支持 ----

    /// 清空解码器内部缓冲区。
    /// Seek 后调用，使解码器从新的关键帧开始解码。
    virtual void flush() = 0;

    // ---- 能力查询 ----

    /// 当前使用的解码后端。
    virtual DecoderBackend backend() const = 0;

    /// 是否为硬件加速解码。
    virtual bool isHardware() const = 0;

    /// 解码器输出的像素格式（如 AV_PIX_FMT_NV12 / AV_PIX_FMT_D3D11 / AV_PIX_FMT_CUDA）。
    /// 仅在 open() 成功后有效。
    virtual int outputPixelFormat() const = 0;
};

} // namespace decoder
} // namespace heisenberg
