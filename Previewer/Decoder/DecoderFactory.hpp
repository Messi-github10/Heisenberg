//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <Decoder/IDecoder.hpp>

#include <memory>

namespace heisenberg {
namespace decoder {

// -- 解码器配置 --

struct DecoderConfig {
    /// 首选解码后端，默认 D3D11 硬件解码。
    DecoderBackend preferred = DecoderBackend::D3D11;

    /// 首选后端不可用时是否自动回退到 Software。
    bool allowFallback = true;
};

// -- 工厂函数 --

/// 根据配置创建一个解码器实例。
/// 若 allowFallback 为 true 且首选后端初始化失败，自动降级为 Software。
/// @return 成功创建的解码器，失败返回 nullptr
std::unique_ptr<IDecoder> createDecoder(const DecoderConfig& config = {});

} // namespace decoder
} // namespace heisenberg
