//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include "AudioSpec.hpp"
#include "Rational.hpp"
#include "VideoSpec.hpp"

#include <cstdint>
#include <vector>

namespace heisenberg {

struct VideoCodecParams {
    VideoSpec spec;
    int bitDepth = 0;
};

struct AudioCodecParams {
    AudioSpec spec;
    int bitDepth = 0;
    int blockAlign = 0;
};

class CodecParams {
public:
    enum Type {
        VIDEO, AUDIO
    };

    enum ID {
        H264, HEVC, VP9, AV1,
        AAC, MP3, OPUS, FLAC, VORBIS,
        UNKNOWN
    };

    Type type = VIDEO;
    ID codecId = UNKNOWN;
    Rational timeBase{1, 1};
    VideoCodecParams video;
    AudioCodecParams audio;

    const uint8_t *extradata() const { return extradata_.data(); }
    int extradataSize() const { return static_cast<int>(extradata_.size()); }
    void setExtradata(const uint8_t *data, size_t n) {
        extradata_.assign(data, data + n);
    }

    int width() const { return video.spec.width; }
    int height() const { return video.spec.height; }
    int fpsNum() const { return video.spec.frameRate.numerator(); }
    int fpsDen() const { return video.spec.frameRate.denominator(); }
    int sarNum() const { return video.spec.sampleAspect.numerator(); }
    int sarDen() const { return video.spec.sampleAspect.denominator(); }
    int sampleRate() const { return audio.spec.sampleRate; }
    int channels() const { return audio.spec.channels; }
    int bitDepth() const { return isAudio() ? audio.bitDepth : video.bitDepth; }
    int blockAlign() const { return audio.blockAlign; }
    int tbNum() const { return timeBase.numerator(); }
    int tbDen() const { return timeBase.denominator(); }

    double toSeconds(int64_t ts) const {
        return timeBase.toDouble() * static_cast<double>(ts);
    }
    int64_t toTimestamp(double sec) const {
        const double scale = timeBase.toDouble();
        return scale == 0.0 ? 0 : static_cast<int64_t>(sec / scale);
    }
    double fps() const { return video.spec.frameRate.toDouble(); }

    bool isVideo() const { return type == VIDEO; }
    bool isAudio() const { return type == AUDIO; }

private:
    std::vector<uint8_t> extradata_;
};

} // namespace heisenberg
