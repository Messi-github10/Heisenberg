//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include "AudioFrame.hpp"
#include <cmath>
#include <cstdint>

namespace heisenberg {
namespace audio {

// 将视频帧位置转换为音频采样点位置
inline int64_t samplesToPosition(double fps, int sampleRate, int64_t position) {
    if (fps <= 0.0 || sampleRate <= 0) return 0;

    return static_cast<int64_t>(
        static_cast<double>(position) * static_cast<double>(sampleRate) / fps + (position < 0 ? -0.5 : 0.5));
}

// 计算某一帧应该包含的采样点数量
inline int samplesForFrame(double fps, int sampleRate, int64_t position) {
    return static_cast<int>(
        samplesToPosition(fps, sampleRate, position + 1) - samplesToPosition(fps, sampleRate, position));
}

inline double frameDuration(double fps) {
    return fps > 0.0 ? 1.0 / fps : 0.0;
}

inline double sampleDuration(int samples, int sampleRate) {
    return sampleRate > 0 ? static_cast<double>(samples) / static_cast<double>(sampleRate) : 0.0;
}

inline double duration(const AudioFrame& frame) {
    return sampleDuration(frame.samples(), frame.spec().sampleRate);
}

inline int applyFrameSamples(AudioFrame& frame, double fps) {
    frame.setSamples(samplesForFrame(fps, frame.spec().sampleRate, frame.position()));
    return frame.samples();
}

inline bool verifySampleCount(const AudioFrame& frame, double fps) {
    return frame.samples() == samplesForFrame(fps, frame.spec().sampleRate, frame.position());
}

} // namespace audio
} // namespace heisenberg
