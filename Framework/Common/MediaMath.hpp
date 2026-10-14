#pragma once

#include "AudioFrame.hpp"
#include "Rational.hpp"

#include <cmath>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace heisenberg {

inline bool isValidTimeBase(AVRational timeBase) {
    return timeBase.num > 0 && timeBase.den > 0;
}

inline bool hasFrameTimestamp(const AVFrame& frame) {
    return frame.pts != AV_NOPTS_VALUE && isValidTimeBase(frame.time_base);
}

inline double frameTimeSeconds(const AVFrame& frame) {
    return static_cast<double>(frame.pts) * av_q2d(frame.time_base);
}

inline double frameTimeMilliseconds(const AVFrame& frame) {
    return frameTimeSeconds(frame) * 1000.0;
}

inline int64_t frameIndexFromTimestamp(const AVFrame& frame, AVRational frameRate) {
    const AVRational frameDuration = av_inv_q(frameRate);
    const auto rounding = static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    return av_rescale_q_rnd(frame.pts, frame.time_base, frameDuration, rounding);
}

inline int64_t timestampFromSeconds(double seconds, AVRational timeBase) {
    if (!isValidTimeBase(timeBase)) return AV_NOPTS_VALUE;
    return static_cast<int64_t>(std::llround(seconds / av_q2d(timeBase)));
}

inline int64_t samplesToPosition(const Rational& frameRate, int sampleRate, int64_t position) {
    if (frameRate.denominator() == 0 || sampleRate <= 0) return 0;
    const double fps = frameRate.toDouble();
    if (fps <= 0.0) return 0;
    return static_cast<int64_t>(
        static_cast<double>(position) * static_cast<double>(sampleRate) / fps
        + (position < 0 ? -0.5 : 0.5));
}

inline int64_t samplesToPosition(double fps, int sampleRate, int64_t position) {
    return samplesToPosition(Rational(static_cast<int>(std::llround(fps * 1000.0)), 1000),
                             sampleRate, position);
}

inline int samplesForFrame(double fps, int sampleRate, int64_t position) {
    return static_cast<int>(
        samplesToPosition(fps, sampleRate, position + 1)
        - samplesToPosition(fps, sampleRate, position));
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

} // namespace heisenberg
