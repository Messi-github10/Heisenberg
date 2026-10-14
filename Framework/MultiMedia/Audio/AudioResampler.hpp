#pragma once
#include <Common/AudioFrame.hpp>
#include <memory>

struct SwrContext;

namespace heisenberg {

// Decoder-worker-only conversion. Planar floating point in and out.
class AudioResampler {
public:
    AudioResampler() = default;
    ~AudioResampler();
    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;
    std::shared_ptr<AudioFrame> convert(const AudioFrame& input, const AudioSpec& output);
    void reset();
private:
    SwrContext* context_ = nullptr;
    int inputRate_ = 0, inputChannels_ = 0, outputRate_ = 0, outputChannels_ = 0;
};

} // namespace heisenberg
