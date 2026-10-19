#pragma once

#include "AudioFrame.hpp"
#include "Rational.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

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
    return hasFrameTimestamp(frame) ? static_cast<double>(frame.pts) * av_q2d(frame.time_base)
        : std::numeric_limits<double>::quiet_NaN();
}

inline double frameTimeMilliseconds(const AVFrame& frame) {
    return frameTimeSeconds(frame) * 1000.0;
}

inline int64_t rescaleTimestamp(int64_t value, AVRational from, AVRational to) {
    if (!isValidTimeBase(from) || !isValidTimeBase(to)) {
        throw std::invalid_argument("Invalid media rescale time base");
    }
    const int64_t result = av_rescale_q_rnd(value, from, to, AV_ROUND_NEAR_INF);
    if (result == std::numeric_limits<int64_t>::min() &&
        !(value == result && av_cmp_q(from, to) == 0)) {
        throw std::overflow_error("Media position rescale overflow");
    }
    return result;
}

inline int64_t frameIndexFromTimestamp(const AVFrame& frame, AVRational frameRate) {
    if (!hasFrameTimestamp(frame) || !isValidTimeBase(frameRate)) return AV_NOPTS_VALUE;
    return rescaleTimestamp(frame.pts, frame.time_base, av_inv_q(frameRate));
}

inline int64_t timestampFromSeconds(double seconds, AVRational timeBase) {
    if (!std::isfinite(seconds) || !isValidTimeBase(timeBase)) return AV_NOPTS_VALUE;
    const long double ticks = static_cast<long double>(seconds) * timeBase.den / timeBase.num;
    const long double rounded = std::round(ticks);
    if (rounded <= static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        rounded >= -static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return AV_NOPTS_VALUE;
    }
    return static_cast<int64_t>(rounded);
}

inline int64_t samplesToPosition(const Rational& frameRate, int sampleRate, int64_t position) {
    if (frameRate.numerator() <= 0 || frameRate.denominator() <= 0 || sampleRate <= 0) return 0;
    
    return rescaleTimestamp(position, av_inv_q(frameRate.toAVRational()), {1, sampleRate});
}

inline int64_t samplesToPosition(double fps, int sampleRate, int64_t position) {
    bool converted = false;
    const auto frameRate = Rational::fromDouble(fps, &converted);
    return converted ? samplesToPosition(frameRate, sampleRate, position) : 0;
}

inline int samplesForFrame(const Rational& fps, int sampleRate, int64_t position) {
    if (position == std::numeric_limits<int64_t>::max()) {
        throw std::overflow_error("Frame end position overflow");
    }

    const auto begin = samplesToPosition(fps, sampleRate, position);
    const auto end = samplesToPosition(fps, sampleRate, position + 1);
    if (end < begin || (begin <= std::numeric_limits<int64_t>::max() -
        std::numeric_limits<int>::max() && end > begin + std::numeric_limits<int>::max())) {
        throw std::overflow_error("Audio samples per frame overflow");
    }
    return static_cast<int>(end - begin);
}

inline int samplesForFrame(double fps, int sampleRate, int64_t position) {
    bool converted = false;
    const auto frameRate = Rational::fromDouble(fps, &converted);
    return converted ? samplesForFrame(frameRate, sampleRate, position) : 0;
}

inline double frameDuration(double fps) {
    return std::isfinite(fps) && fps > 0.0 ? 1.0 / fps : 0.0;
}

inline double sampleDuration(int samples, int sampleRate) {
    return sampleRate > 0 ? static_cast<double>(samples) / sampleRate : 0.0;
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
