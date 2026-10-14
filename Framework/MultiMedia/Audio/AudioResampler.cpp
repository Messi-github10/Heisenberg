#include "AudioResampler.hpp"
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
}
std::shared_ptr<AudioFrame> AudioResampler::convert(const AudioFrame& input, const AudioSpec& output) {
    if (!input.spec().valid() || !output.valid() || input.samples() <= 0) return {};
    if (!context_ || inputRate_ != input.spec().sampleRate || inputChannels_ != input.spec().channels
        || outputRate_ != output.sampleRate || outputChannels_ != output.channels) {
        reset();
        inputRate_ = input.spec().sampleRate;
        inputChannels_ = input.spec().channels;
        outputRate_ = output.sampleRate;
        outputChannels_ = output.channels;
        AVChannelLayout in{}, out{};
        av_channel_layout_default(&in, inputChannels_);
        av_channel_layout_default(&out, outputChannels_);
        const int result = swr_alloc_set_opts2(&context_, &out, AV_SAMPLE_FMT_FLTP, outputRate_,
                                               &in, AV_SAMPLE_FMT_FLTP, inputRate_, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (result < 0 || !context_ || swr_init(context_) < 0) { reset(); return {}; }
    }
    const int capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(context_, inputRate_) + input.samples(), outputRate_, inputRate_, AV_ROUND_UP));
    if (capacity <= 0) return {};
    std::vector<std::vector<float>> planes(outputChannels_, std::vector<float>(capacity));
    std::vector<uint8_t*> out(outputChannels_);
    std::vector<const uint8_t*> in(inputChannels_);
    for (int c = 0; c < outputChannels_; ++c) out[c] = reinterpret_cast<uint8_t*>(planes[c].data());
    for (int c = 0; c < inputChannels_; ++c) in[c] = reinterpret_cast<const uint8_t*>(input.channelRo(c));
    const int samples = swr_convert(context_, out.data(), capacity, in.data(), input.samples());
    if (samples <= 0) return {};
    auto result = std::make_shared<AudioFrame>();
    result->setSpec(output);
    result->setSamples(samples, true);
    if (input.hasPosition()) result->setPosition(av_rescale_q(input.position(),
        AVRational{1, inputRate_}, AVRational{1, outputRate_}));
    for (int c = 0; c < outputChannels_; ++c)
        std::copy_n(planes[c].data(), samples, result->channelRw(c));
    return result;
}
} // namespace heisenberg
