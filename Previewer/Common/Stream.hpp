//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <string>

#include "Codec.hpp"

namespace heisenberg {

class Stream {
public:
    enum Type {
        VIDEO, AUDIO
    };

    bool isVideo() const { return type == VIDEO; }
    bool isAudio() const { return type == AUDIO; }

    int index = -1;
    int demuxerId = -1;
    Type type = VIDEO;

    CodecParams codec;

    std::string language;
    std::string title;

    bool isDefault = false;
    bool isForced = false;
};

} // namespace heisenberg
