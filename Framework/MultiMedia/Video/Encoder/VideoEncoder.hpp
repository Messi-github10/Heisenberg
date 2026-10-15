#pragma once
#include "Encoding.hpp"
#include <cstddef>

struct AVFrame;

namespace heisenberg {

enum class VideoEncodingCodec { H264, H265 };
enum class EncodedRgbFormat { RGBA8, RGB24, RGBA32F, RGBA16F };

struct VideoEncoderSettings {
    int width = 0;
    int height = 0;
    AVRational frameRate{24, 1};
    VideoEncodingCodec codec = VideoEncodingCodec::H264;
    int crf = -1; // -1 selects H264=20 / H265=24; otherwise 0..51
    std::string preset = "medium";
    AVRational sampleAspect{1, 1};
};

// ODT is already applied: RGB is full-range, nonlinear BT.709 R'G'B',
// normalized [0,1] for float; opaque/composited (alpha is ignored). No ODT here.
// Floats are explicitly unpacked on CPU, not passed to swscale. NaN/Inf fails;
// finite excursions are clipped. Half/float are native-endian IEEE-754.
struct EncodedRgbFrameView {
    const void* data = nullptr;
    std::ptrdiff_t rowBytes = 0; // positive; data points at the first/top row
    int width = 0;
    int height = 0;
    EncodedRgbFormat format = EncodedRgbFormat::RGBA32F;
};

class VideoEncoder {
public:
    explicit VideoEncoder(const VideoEncoderSettings& settings);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    CodecParameters codecParameters() const;
    AVRational timeBase() const noexcept;
    // pts is a frame index in inverse-frameRate units, strictly increasing.
    EncodedPackets encode(const EncodedRgbFrameView& rgb, int64_t pts);
    // CPU packed RGB24/RGBA/RGBAF32/RGBAF16 only; checks dimensions/stride.
    EncodedPackets encode(const AVFrame& rgb, int64_t pts);
    // Sends NULL and receives to EOF. Repeated drain is harmless; encode after
    // drain fails. Returned packets retain original B-frame PTS/DTS.
    EncodedPackets drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
