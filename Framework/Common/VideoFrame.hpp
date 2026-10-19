#pragma once

#include "VideoSpec.hpp"
#include <cstdint>
#include <memory>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {

class VideoFrame {
public:
    VideoFrame() = default;
    explicit VideoFrame(std::shared_ptr<AVFrame> image,
                        int64_t outputPosition = -1,
                        Rational frameRate = {})
        : image_(std::move(image)), position_(outputPosition) {
        spec_.frameRate = frameRate;
        if (!image_) return;
        const auto& frame = *image_;
        spec_.width = frame.width;
        spec_.height = frame.height;
        spec_.format = static_cast<AVPixelFormat>(frame.format);
        spec_.sampleAspect = frame.sample_aspect_ratio.num > 0 &&
            frame.sample_aspect_ratio.den > 0
            ? Rational(frame.sample_aspect_ratio) : Rational(1, 1);
        spec_.timeBase = Rational(frame.time_base);
        spec_.colorPrimaries = frame.color_primaries;
        spec_.colorTransfer = frame.color_trc;
        spec_.colorSpace = frame.colorspace;
        spec_.colorRange = frame.color_range;
        spec_.chromaLocation = frame.chroma_location;
        pts_ = frame.pts;
        duration_ = frame.duration;
    }

    static std::shared_ptr<VideoFrame> wrap(std::shared_ptr<AVFrame> image,
                                           int64_t position = -1,
                                           Rational frameRate = {}) {
        if (!image) return {};
        return std::make_shared<VideoFrame>(std::move(image), position, frameRate);
    }

    const std::shared_ptr<AVFrame>& native() const {
        return image_;
    }

    const std::shared_ptr<AVFrame>& image() const {
        return native();
    }

    AVFrame* get() const {
        return image_.get();
    }

    explicit operator bool() const {
        return static_cast<bool>(image_);
    }

    const VideoSpec& spec() const {
        return spec_;
    }

    int64_t pts() const {
        return pts_;
    }

    int64_t duration() const {
        return duration_;
    }

    const Rational& timeBase() const {
        return spec_.timeBase;
    }

    bool hasTimestamp() const {
        return pts_ != AV_NOPTS_VALUE && spec_.timeBase.numerator() > 0 &&
               spec_.timeBase.denominator() > 0;
    }

    int64_t position() const {
        return position_;
    }

    bool hasPosition() const {
        return position_ != -1;
    }

    std::shared_ptr<VideoFrame> withPosition(int64_t position) const {
        auto result = std::make_shared<VideoFrame>(*this);
        result->position_ = position;
        return result;
    }

    std::shared_ptr<VideoFrame> clone() const {
        if (!image_) return {};
        AVFrame* copy = av_frame_clone(image_.get());
        if (!copy) return {};
        auto result = std::make_shared<VideoFrame>(*this);
        result->image_ = std::shared_ptr<AVFrame>(copy, [](AVFrame* frame) {
            av_frame_free(&frame);
        });
        return result;
    }

private:
    std::shared_ptr<AVFrame> image_;
    VideoSpec spec_;
    int64_t pts_ = AV_NOPTS_VALUE;
    int64_t duration_ = 0;
    int64_t position_ = -1;
};

} // namespace heisenberg
