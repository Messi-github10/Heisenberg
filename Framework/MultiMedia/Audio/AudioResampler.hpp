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
    std::shared_ptr<AudioFrame> drain();
    void reset();
private:
    std::shared_ptr<AudioFrame> convertSamples(const AudioFrame* input, int capacity);
    SwrContext* context_ = nullptr;
    int inputRate_ = 0, inputChannels_ = 0, outputRate_ = 0, outputChannels_ = 0;
    AudioSpec outputSpec_;
    int64_t nextPosition_ = 0, expectedInputPosition_ = 0;
    bool positioned_ = false, inputPositionKnown_ = false;
};

} // namespace heisenberg
