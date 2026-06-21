//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <cstdint>
#include <vector>

namespace heisenberg {

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

    const uint8_t *extradata() const { return extradata_.data(); }
    int extradataSize() const { return static_cast<int>(extradata_.size()); }
    void setExtradata(const uint8_t *data, size_t n) {
        extradata_.assign(data, data + n);
    }

    int width = 0;
    int height = 0;
    int fpsNum = 0;
    int fpsDen = 1;
    int sarNum = 1;
    int sarDen = 1;

    int sampleRate = 0;
    int channels = 0;
    int bitDepth = 0;
    int blockAlign = 0;

    int tbNum = 1;
    int tbDen = 1;

    double toSeconds(int64_t ts) const {
        return static_cast<double>(ts) * tbNum / tbDen;
    }
    int64_t toTimestamp(double sec) const {
        return static_cast<int64_t>(sec * tbDen / tbNum);
    }
    double fps() const {
        return fpsDen ? static_cast<double>(fpsNum) / fpsDen : 0.0;
    }

    bool isVideo() const { return type == VIDEO; }
    bool isAudio() const { return type == AUDIO; }

private:
    std::vector<uint8_t> extradata_;
};

} // namespace heisenberg
