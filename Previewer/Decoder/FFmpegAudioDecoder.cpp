//
// Created by NiceFold on 2026/7/27.
//

#include <Decoder/FFmpegAudioDecoder.hpp>
#include <Common/AudioMath.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

namespace heisenberg {
namespace decoder {

struct FFmpegAudioDecoder::Impl {
    std::vector<float> planarBuffer;
    AudioSpec spec_;
    int64_t totalSamplesPerChannel = 0;

    void close() {
        planarBuffer.clear();
        totalSamplesPerChannel = 0;
        spec_ = AudioSpec{};
    }

    bool openFile(const std::string& path) {
        AVFormatContext* fmtCtx = nullptr;
        int ret = avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr);
        if (ret < 0 || !fmtCtx) return false;

        ret = avformat_find_stream_info(fmtCtx, nullptr);
        if (ret < 0) {
            avformat_close_input(&fmtCtx);
            return false;
        }

        int audioIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioIndex < 0) {
            avformat_close_input(&fmtCtx);
            return false;
        }

        AVStream* avStream = fmtCtx->streams[audioIndex];
        const AVCodec* codec = avcodec_find_decoder(avStream->codecpar->codec_id);
        if (!codec) {
            avformat_close_input(&fmtCtx);
            return false;
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            avformat_close_input(&fmtCtx);
            return false;
        }

        ret = avcodec_parameters_to_context(codecCtx, avStream->codecpar);
        if (ret < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        ret = avcodec_open2(codecCtx, codec, nullptr);
        if (ret < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        SwrContext* swr = nullptr;
        AVChannelLayout outChLayout = {};
        av_channel_layout_default(&outChLayout, codecCtx->ch_layout.nb_channels);

        ret = swr_alloc_set_opts2(&swr,
                                  &outChLayout, AV_SAMPLE_FMT_FLTP, codecCtx->sample_rate,
                                  &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
                                  0, nullptr);
        av_channel_layout_uninit(&outChLayout);

        if (ret < 0 || !swr) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        ret = swr_init(swr);
        if (ret < 0) {
            swr_free(&swr);
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            return false;
        }

        spec_.sampleRate = codecCtx->sample_rate;
        spec_.channels   = codecCtx->ch_layout.nb_channels;
        spec_.layout     = defaultLayout(spec_.channels);

        const int channels = spec_.channels;
        std::vector<std::vector<float>> chanBufs(channels);

        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt  = av_packet_alloc();

        std::vector<uint8_t*> dstPlanes(channels);
        std::vector<float>     chunkBuf;

        while (av_read_frame(fmtCtx, pkt) >= 0) {
            if (pkt->stream_index == audioIndex) {
                ret = avcodec_send_packet(codecCtx, pkt);
                if (ret < 0) {
                    av_packet_unref(pkt);
                    continue;
                }

                while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                    const int dstSamples = frame->nb_samples;

                    chunkBuf.resize(static_cast<size_t>(dstSamples) * channels);
                    for (int ch = 0; ch < channels; ++ch) {
                        dstPlanes[ch] = reinterpret_cast<uint8_t*>(
                            chunkBuf.data() + static_cast<size_t>(ch) * dstSamples);
                    }

                    const int converted = swr_convert(
                        swr,
                        dstPlanes.data(), dstSamples,
                        const_cast<const uint8_t**>(frame->data), frame->nb_samples);

                    if (converted > 0) {
                        for (int ch = 0; ch < channels; ++ch) {
                            const float* src = reinterpret_cast<const float*>(dstPlanes[ch]);
                            chanBufs[ch].insert(chanBufs[ch].end(), src, src + converted);
                        }
                        totalSamplesPerChannel += converted;
                    }

                    av_frame_unref(frame);
                }
            }
            av_packet_unref(pkt);
        }

        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            const int dstSamples = frame->nb_samples;

            chunkBuf.resize(static_cast<size_t>(dstSamples) * channels);
            for (int ch = 0; ch < channels; ++ch) {
                dstPlanes[ch] = reinterpret_cast<uint8_t*>(
                    chunkBuf.data() + static_cast<size_t>(ch) * dstSamples);
            }

            const int converted = swr_convert(
                swr,
                dstPlanes.data(), dstSamples,
                const_cast<const uint8_t**>(frame->data), frame->nb_samples);

            if (converted > 0) {
                for (int ch = 0; ch < channels; ++ch) {
                    const float* src = reinterpret_cast<const float*>(dstPlanes[ch]);
                    chanBufs[ch].insert(chanBufs[ch].end(), src, src + converted);
                }
                totalSamplesPerChannel += converted;
            }

            av_frame_unref(frame);
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);

        if (totalSamplesPerChannel > 0) {
            planarBuffer.reserve(static_cast<size_t>(totalSamplesPerChannel) * channels);
            for (int ch = 0; ch < channels; ++ch) {
                planarBuffer.insert(planarBuffer.end(),
                                    chanBufs[ch].begin(), chanBufs[ch].end());
            }
        }

        return totalSamplesPerChannel > 0;
    }
};

FFmpegAudioDecoder::FFmpegAudioDecoder()
    : impl_(std::make_unique<Impl>()) {}

FFmpegAudioDecoder::~FFmpegAudioDecoder() {
    impl_->close();
}

bool FFmpegAudioDecoder::open(const std::string& filePath) {
    impl_->close();
    return impl_->openFile(filePath);
}

void FFmpegAudioDecoder::close() {
    impl_->close();
}

bool FFmpegAudioDecoder::isOpen() const {
    return impl_->totalSamplesPerChannel > 0;
}

AudioSpec FFmpegAudioDecoder::spec() const {
    return impl_->spec_;
}

int64_t FFmpegAudioDecoder::totalFrames(double fps) const {
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

int64_t FFmpegAudioDecoder::totalSamples() const {
    return impl_->totalSamplesPerChannel;
}

const float* FFmpegAudioDecoder::planarData() const {
    return impl_->planarBuffer.data();
}

double FFmpegAudioDecoder::duration() const {
    if (!isOpen()) return 0.0;
    return static_cast<double>(impl_->totalSamplesPerChannel)
         / static_cast<double>(impl_->spec_.sampleRate);
}

bool FFmpegAudioDecoder::decode(AudioFrame& frame, int64_t position, double fps) {
    if (!isOpen() || fps <= 0.0) return false;

    const int samples = audio::samplesForFrame(fps, impl_->spec_.sampleRate, position);
    if (samples <= 0) return false;

    const int64_t startSample = audio::samplesToPosition(
        fps, impl_->spec_.sampleRate, position);

    const int64_t endSample   = startSample + samples;
    const int64_t available   = impl_->totalSamplesPerChannel;
    const int64_t clampedEnd  = std::min(endSample, available);
    const int     actualCount = static_cast<int>(clampedEnd - startSample);

    if (actualCount <= 0) {
        frame.setSpec(impl_->spec_);
        frame.setSamples(samples);
        frame.setPosition(position);
        frame.assign(static_cast<size_t>(samples) * impl_->spec_.channels, 0.0f);
        return true;
    }

    frame.setSpec(impl_->spec_);
    frame.setSamples(actualCount);
    frame.setPosition(position);
    frame.resize(static_cast<size_t>(actualCount) * impl_->spec_.channels);

    for (int ch = 0; ch < impl_->spec_.channels; ++ch) {
        const float* src = impl_->planarBuffer.data()
                         + ch * impl_->totalSamplesPerChannel
                         + startSample;
        float* dst = frame.channelRw(ch);
        std::memcpy(dst, src, static_cast<size_t>(actualCount) * sizeof(float));
    }

    if (actualCount < samples) {
        const int missing = samples - actualCount;
        for (int ch = 0; ch < impl_->spec_.channels; ++ch) {
            std::fill(frame.channelRw(ch) + actualCount,
                      frame.channelRw(ch) + samples, 0.0f);
        }
        frame.setSamples(samples);
    }

    return true;
}

bool FFmpegAudioDecoder::seek(double timeSeconds) {
    (void)timeSeconds;
    return isOpen();
}

} // namespace decoder
} // namespace heisenberg
