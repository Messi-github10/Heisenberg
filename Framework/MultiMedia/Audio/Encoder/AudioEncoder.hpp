#pragma once
#include <MultiMedia/Video/Encoder/Encoding.hpp>

struct AVFrame;

namespace heisenberg {
class AudioFrame;

class AudioEncoder {
public:
    // AAC-LC, 192 kbps, 48 kHz stereo FLTP, global header enabled.
    AudioEncoder();
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;
    CodecParameters codecParameters() const;
    AVRational timeBase() const noexcept; // {1,48000}
    int frameSize() const noexcept;
    int initialPadding() const noexcept;
    int64_t samplesSubmitted() const noexcept; // real input, excludes padding
    // Sequential PCM starting at sample zero. Known positions must equal
    // samplesSubmitted(); no timeline mapping, gap filling or resampling here.
    EncodedPackets encode(const AudioFrame& pcm);
    EncodedPackets encode(const AVFrame& pcm); // 48 kHz stereo AV_SAMPLE_FMT_FLTP
    EncodedPackets encode(const float* left, const float* right, int samples);
    // FIFO -> last partial frame -> NULL -> EOF. Holds one packet until its
    // successor/drain so final skip-samples and exact real duration survive.
    // Keeps negative initial AAC PTS; returns initial/tail SKIP_SAMPLES side data.
    EncodedPackets drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
