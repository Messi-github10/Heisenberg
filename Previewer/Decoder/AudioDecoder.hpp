//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include <Decoder/IAudioDecoder.hpp>
#include <memory>

namespace heisenberg {
namespace decoder {

class AudioDecoder final : public IAudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder() override;

    bool open(const std::string& filePath) override;
    void close() override;
    bool isOpen() const override;

    AudioSpec spec() const override;
    int64_t totalFrames(double fps) const override;
    int64_t totalSamples() const override;
    const float* planarData() const override;
    double duration() const override;

    bool decode(AudioFrame& frame, int64_t position, double fps) override;
    bool seek(double timeSeconds) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace decoder
} // namespace heisenberg
