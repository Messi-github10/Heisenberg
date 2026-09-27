//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include <Common/AudioFrame.hpp>

#include <cstdint>
#include <string>

namespace heisenberg {
namespace decoder {

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual bool open(const std::string& filePath) = 0;

    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    virtual AudioSpec spec() const = 0;

    virtual int64_t totalFrames(double fps) const = 0;

    virtual double duration() const = 0;

    virtual int64_t totalSamples() const = 0;

    virtual const float* planarData() const = 0;

    virtual bool decode(AudioFrame& frame, int64_t position, double fps) = 0;

    virtual bool seek(double timeSeconds) = 0;
};

} // namespace decoder
} // namespace heisenberg
