//
// Created by NiceFold on 2026/7/27.
//

#include <Decoder/AudioDecoder.hpp>

extern "C" {
#include <miniaudio.h>
}

#include <Common/AudioMath.hpp>
#include <Common/AudioSpec.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace heisenberg {
namespace decoder {

struct AudioDecoder::Impl {
    ma_decoder decoder_{};
    bool isOpen_ = false;
    std::vector<float> planarBuffer;

    AudioSpec spec_;
    int64_t totalSamplesPerChannel = 0;

    void close() {
        if (isOpen_) {
            ma_decoder_uninit(&decoder_);
            isOpen_ = false;
        }
        planarBuffer.clear();
        totalSamplesPerChannel = 0;
        spec_ = AudioSpec{};
    }

    bool readAllFrames() {
        ma_uint64 totalPcmFrames = 0;
        ma_decoder_get_length_in_pcm_frames(&decoder_, &totalPcmFrames);
        if (totalPcmFrames == 0) return false;

        const ma_uint32 channels = decoder_.outputChannels;
        const ma_uint64 totalFloats = totalPcmFrames * channels;

        // Temporary interleaved buffer
        std::vector<float> interleaved;
        interleaved.resize(static_cast<size_t>(totalFloats));

        ma_uint64 framesRead = 0;
        ma_result result = ma_decoder_read_pcm_frames(
            &decoder_,
            interleaved.data(),
            totalPcmFrames,
            &framesRead
        );

        if (result != MA_SUCCESS && result != MA_AT_END) {
            return false;
        }

        const ma_uint64 actualFrames = framesRead;
        planarBuffer.resize(static_cast<size_t>(actualFrames * channels));

        for (ma_uint64 frame = 0; frame < actualFrames; ++frame) {
            for (ma_uint32 ch = 0; ch < channels; ++ch) {
                const size_t interleavedIdx = static_cast<size_t>(frame * channels + ch);
                const size_t planarIdx = static_cast<size_t>(ch * actualFrames + frame);
                planarBuffer[planarIdx] = interleaved[interleavedIdx];
            }
        }

        totalSamplesPerChannel = static_cast<int64_t>(actualFrames);
        return true;
    }
};

AudioDecoder::AudioDecoder()
    : impl_(std::make_unique<Impl>()) {}

AudioDecoder::~AudioDecoder() {
    impl_->close();
}

bool AudioDecoder::open(const std::string& filePath) {
    impl_->close();

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32,
        0,
        0
    );

    ma_result result = ma_decoder_init_file(filePath.c_str(), &config, &impl_->decoder_);
    if (result != MA_SUCCESS) {
        return false;
    }

    impl_->isOpen_ = true;

    if (!impl_->readAllFrames()) {
        impl_->close();
        return false;
    }

    impl_->spec_.sampleRate = static_cast<int>(impl_->decoder_.outputSampleRate);
    impl_->spec_.channels   = static_cast<int>(impl_->decoder_.outputChannels);
    impl_->spec_.layout     = defaultLayout(impl_->spec_.channels);

    ma_decoder_uninit(&impl_->decoder_);
    impl_->isOpen_ = false;

    return true;
}

void AudioDecoder::close() {
    impl_->close();
}

bool AudioDecoder::isOpen() const {
    return impl_->totalSamplesPerChannel > 0;
}

AudioSpec AudioDecoder::spec() const {
    return impl_->spec_;
}

int64_t AudioDecoder::totalFrames(double fps) const {
    if (!isOpen() || fps <= 0.0) return 0;

    int64_t frame = 0;
    int64_t accumulated = 0;

    while (accumulated < impl_->totalSamplesPerChannel) {
        int samples = audio::samplesForFrame(fps, impl_->spec_.sampleRate, frame);
        if (samples <= 0) break;
        accumulated += samples;
        ++frame;
    }

    return frame;
}

int64_t AudioDecoder::totalSamples() const {
    return impl_->totalSamplesPerChannel;
}

const float* AudioDecoder::planarData() const {
    return impl_->planarBuffer.data();
}

double AudioDecoder::duration() const {
    if (!isOpen()) return 0.0;

    return static_cast<double>(impl_->totalSamplesPerChannel)
         / static_cast<double>(impl_->spec_.sampleRate);
}

bool AudioDecoder::decode(AudioFrame& frame, int64_t position, double fps) {
    if (!isOpen()) return false;
    if (fps <= 0.0) return false;

    const int samples = audio::samplesForFrame(fps, impl_->spec_.sampleRate, position);
    if (samples <= 0) return false;

    const int64_t startSample = audio::samplesToPosition(
        fps, impl_->spec_.sampleRate, position);

    const int64_t endSample = startSample + samples;
    const int64_t available = impl_->totalSamplesPerChannel;
    const int64_t clampedEnd = std::min(endSample, available);
    const int actualSamples = static_cast<int>(clampedEnd - startSample);

    if (actualSamples <= 0) {
        frame.setSpec(impl_->spec_);
        frame.setSamples(samples);
        frame.setPosition(position);
        frame.assign(static_cast<size_t>(samples) * impl_->spec_.channels, 0.0f);
        return true;
    }

    frame.setSpec(impl_->spec_);
    frame.setSamples(actualSamples);
    frame.setPosition(position);
    frame.resize(static_cast<size_t>(actualSamples) * impl_->spec_.channels);

    for (int ch = 0; ch < impl_->spec_.channels; ++ch) {
        const float* src = impl_->planarBuffer.data()
                         + ch * impl_->totalSamplesPerChannel
                         + startSample;
        float* dst = frame.channelRw(ch);
        std::memcpy(dst, src, static_cast<size_t>(actualSamples) * sizeof(float));
    }

    if (actualSamples < samples) {
        const int missing = samples - actualSamples;
        for (int ch = 0; ch < impl_->spec_.channels; ++ch) {
            float* dst = frame.channelRw(ch) + actualSamples;
            std::fill(dst, dst + missing, 0.0f);
        }
        frame.setSamples(samples);
    }

    return true;
}

bool AudioDecoder::seek(double timeSeconds) {
    if (!isOpen()) return false;

    // TODO: 当前为预解码模式，seek 由上层 PlaybackController 直接
    //       修改 audioSamplePos 完成，此方法为占位实现。
    //       切换到流式解码后，需要在此处调用 ma_decoder_seek_to_pcm_frame()
    //       将 decoder 的文件读取位置跳到目标采样帧，并清空内部 buffer。
    (void)timeSeconds;
    return true;
}

} // namespace decoder
} // namespace heisenberg
