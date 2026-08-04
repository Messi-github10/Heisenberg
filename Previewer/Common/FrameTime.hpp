#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#include <cmath>
#include <cstdint>

namespace heisenberg {

inline bool isValidTimeBase(AVRational timeBase) {
    return timeBase.num > 0 && timeBase.den > 0;
}

inline bool hasFrameTimestamp(const AVFrame& frame) {
    return frame.pts != AV_NOPTS_VALUE
        && isValidTimeBase(frame.time_base);
}

inline double frameTimeSeconds(const AVFrame& frame) {
    return static_cast<double>(frame.pts) * av_q2d(frame.time_base);
}

inline double frameTimeMilliseconds(const AVFrame& frame) {
    return frameTimeSeconds(frame) * 1000.0;
}

inline int64_t frameIndexFromTimestamp(const AVFrame& frame,
                                       AVRational frameRate) {
    const AVRational frameDuration = av_inv_q(frameRate);
    const auto rounding = static_cast<AVRounding>(
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    return av_rescale_q_rnd(frame.pts,
                            frame.time_base,
                            frameDuration,
                            rounding);
}

inline int64_t timestampFromSeconds(double seconds, AVRational timeBase) {
    if (!isValidTimeBase(timeBase)) return AV_NOPTS_VALUE;
    return static_cast<int64_t>(std::llround(seconds / av_q2d(timeBase)));
}

} // namespace heisenberg
