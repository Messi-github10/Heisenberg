#include "AudioResampler.hpp"
#include <cmath>
#include <vector>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace heisenberg {
AudioResampler::~AudioResampler() { reset(); }
void AudioResampler::reset() {
    swr_free(&context_);
    inputRate_ = inputChannels_ = outputRate_ = outputChannels_ = 0;
    nextPosition_ = expectedInputPosition_ = 0;
    positioned_ = inputPositionKnown_ = false;
}
std::shared_ptr<AudioFrame> AudioResampler::convert(const AudioFrame& input, const AudioSpec& output) {
    if (!input.spec().valid() || !output.valid() || input.samples() <= 0) return {};
    // Preserve filter continuity for rounding jitter, but do not bridge a real
    // timestamp discontinuity with samples from the preceding segment.
    const bool discontinuity = input.hasPosition() && inputPositionKnown_
        && std::abs(input.position() - expectedInputPosition_) > 2;
    if (!context_ || discontinuity || inputRate_ != input.spec().sampleRate
        || inputChannels_ != input.spec().channels || outputRate_ != output.sampleRate
        || outputChannels_ != output.channels) {
        reset();
        inputRate_ = input.spec().sampleRate;
        inputChannels_ = input.spec().channels;
        outputRate_ = output.sampleRate;
        outputChannels_ = output.channels;
        outputSpec_ = output;
        AVChannelLayout in{}, out{};
        av_channel_layout_default(&in, inputChannels_);
        av_channel_layout_default(&out, outputChannels_);
        const int result = swr_alloc_set_opts2(&context_, &out, AV_SAMPLE_FMT_FLTP, outputRate_,
                                               &in, AV_SAMPLE_FMT_FLTP, inputRate_, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (result < 0 || !context_ || swr_init(context_) < 0) { reset(); return {}; }
    }
    if (input.hasPosition()) {
        if (!positioned_) {
            const int64_t delay = swr_get_delay(context_, inputRate_);
            nextPosition_ = av_rescale_q(input.position() - delay,
                AVRational{1, inputRate_}, AVRational{1, outputRate_});
            positioned_ = true;
        }
        expectedInputPosition_ = input.position() + input.samples();
        inputPositionKnown_ = true;
    } else if (inputPositionKnown_) expectedInputPosition_ += input.samples();
    const int capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(context_, inputRate_) + input.samples(), outputRate_, inputRate_, AV_ROUND_UP));
    return convertSamples(&input, capacity);
}
std::shared_ptr<AudioFrame> AudioResampler::drain() {
    if (!context_) return {};
    const int capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(context_, inputRate_), outputRate_, inputRate_, AV_ROUND_UP));
    return convertSamples(nullptr, capacity);
}
std::shared_ptr<AudioFrame> AudioResampler::convertSamples(const AudioFrame* input, int capacity) {
    if (capacity <= 0) return {};
    std::vector<std::vector<float>> planes(outputChannels_, std::vector<float>(capacity));
    std::vector<uint8_t*> out(outputChannels_);
    std::vector<const uint8_t*> in(inputChannels_);
    for (int c = 0; c < outputChannels_; ++c) out[c] = reinterpret_cast<uint8_t*>(planes[c].data());
    if (input) for (int c = 0; c < inputChannels_; ++c)
        in[c] = reinterpret_cast<const uint8_t*>(input->channelRo(c));
    const int samples = swr_convert(context_, out.data(), capacity,
        input ? in.data() : nullptr, input ? input->samples() : 0);
    if (samples <= 0) return {};
    auto result = std::make_shared<AudioFrame>();
    result->setSpec(outputSpec_);
    result->setSamples(samples, true);
    if (positioned_) result->setPosition(nextPosition_);
    nextPosition_ += samples;
    for (int c = 0; c < outputChannels_; ++c)
        std::copy_n(planes[c].data(), samples, result->channelRw(c));
    return result;
}
} // namespace heisenberg
