//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include <cstdint>

namespace heisenberg {

enum class AudioFormat : uint8_t {
    None   = 0,
    U8     = 1,
    S16    = 2,
    S32    = 3,
    F32    = 4,
    S32LE  = 5,
    F32LE  = 6,
};

enum class ChannelLayout : uint8_t {
    Auto        = 0,
    Independent = 1,
    Mono        = 2,
    Stereo      = 3,
    _2_1        = 4,
    _5_1        = 5,
    _7_1        = 6,
    _7_1_Wide   = 7,
};

struct AudioSpec {
    int sampleRate  = 48000;
    int channels    = 2;
    ChannelLayout layout = ChannelLayout::Auto;

    bool valid() const { return sampleRate > 0 && channels > 0; }

    bool operator==(const AudioSpec& rhs) const {
        return sampleRate == rhs.sampleRate && channels == rhs.channels;
    }
    bool operator!=(const AudioSpec& rhs) const { return !(*this == rhs); }
};

// 根据布局获取声道数
inline constexpr int channelCount(ChannelLayout layout) {
    switch (layout) {
    case ChannelLayout::Mono:   return 1;
    case ChannelLayout::Stereo: return 2;
    case ChannelLayout::_2_1:   return 3;
    case ChannelLayout::_5_1:   return 6;
    case ChannelLayout::_7_1:   return 8;
    case ChannelLayout::_7_1_Wide: return 8;
    default: return 0;
    }
}

// 根据声道数推断默认布局
inline ChannelLayout defaultLayout(int channels) {
    switch (channels) {
    case 1:  return ChannelLayout::Mono;
    case 2:  return ChannelLayout::Stereo;
    case 3:  return ChannelLayout::_2_1;
    case 6:  return ChannelLayout::_5_1;
    case 8:  return ChannelLayout::_7_1;
    default: return ChannelLayout::Independent;
    }
}

// 获取每种格式的样本字节数
inline constexpr int bytesPerSample(AudioFormat fmt) {
    switch (fmt) {
    case AudioFormat::U8:    return 1;
    case AudioFormat::S16:   return 2;
    case AudioFormat::S32:
    case AudioFormat::S32LE:
    case AudioFormat::F32:
    case AudioFormat::F32LE: return 4;
    default:                 return 0;
    }
}

} // namespace heisenberg
