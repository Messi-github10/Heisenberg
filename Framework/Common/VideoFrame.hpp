#pragma once

#include "VideoSpec.hpp"

#include <memory>

struct AVFrame;

namespace heisenberg {

// The decoded image stays in AVFrame. This type owns that image and carries
// the specification and timing used by the rest of the pipeline.
class VideoFrame {
public:
    VideoFrame() = default;
    explicit VideoFrame(std::shared_ptr<AVFrame> image) : image_(std::move(image)) {}

    const std::shared_ptr<AVFrame>& image() const { return image_; }
    AVFrame* get() const { return image_.get(); }
    explicit operator bool() const { return static_cast<bool>(image_); }

    const VideoSpec& spec() const { return spec_; }
    void setSpec(const VideoSpec& spec) { spec_ = spec; }

    int64_t position() const { return position_; }
    bool hasPosition() const { return position_ >= 0; }
    void setPosition(int64_t position) { position_ = position; }

private:
    std::shared_ptr<AVFrame> image_;
    VideoSpec spec_;
    int64_t position_ = -1;
};

} // namespace heisenberg
