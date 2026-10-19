#include "FrameRenderer.hpp"
#include "VideoNormalizer.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace heisenberg {
namespace {

std::shared_ptr<AVFrame> makeBlack(const TimelineSettings& settings, std::string* error) {
    AVFrame* raw = av_frame_alloc();
    if (!raw) {
        if (error) *error = "Cannot allocate background frame";
        return {};
    }
    auto frame = std::shared_ptr<AVFrame>(raw, [](AVFrame* value) { av_frame_free(&value); });
    const auto& spec = settings.videoSpec();
    raw->width = spec.width;
    raw->height = spec.height;
    raw->format = spec.format;
    raw->sample_aspect_ratio = spec.sampleAspect.toAVRational();
    raw->color_primaries = spec.colorPrimaries;
    raw->color_trc = spec.colorTransfer;
    raw->colorspace = AVCOL_SPC_RGB;
    raw->color_range = spec.colorRange;
    if (av_frame_get_buffer(raw, 32) < 0) {
        if (error) *error = "Cannot allocate background pixels";
        return {};
    }
    const bool limited = settings.range() == ColorRange::Limited;
    for (int y = 0; y < raw->height; ++y) {
        uint8_t* row = raw->data[0] + static_cast<size_t>(y) * raw->linesize[0];
        for (int x = 0; x < raw->width; ++x) {
            switch (settings.workingFormat()) {
                case WorkingFormat::Rgba8: {
                    const uint8_t pixel[4] = {static_cast<uint8_t>(limited ? 16 : 0),
                        static_cast<uint8_t>(limited ? 16 : 0),
                        static_cast<uint8_t>(limited ? 16 : 0), 255};
                    std::memcpy(row + static_cast<size_t>(x) * 4, pixel, sizeof(pixel));
                    break;
                }
                case WorkingFormat::Rgba16f: {
                    // IEEE half encoding of 16/255 rounded nearest and 1.0.
                    const uint16_t black = limited ? 0x2c04 : 0;
                    const uint16_t pixel[4] = {black, black, black, 0x3c00};
                    std::memcpy(row + static_cast<size_t>(x) * 8, pixel, sizeof(pixel));
                    break;
                }
                case WorkingFormat::Rgba32f: {
                    const float black = limited ? 16.0f / 255.0f : 0.0f;
                    const float pixel[4] = {black, black, black, 1.0f};
                    std::memcpy(row + static_cast<size_t>(x) * 16, pixel, sizeof(pixel));
                    break;
                }
            }
        }
    }
    return frame;
}

} // namespace

struct FrameRenderer::Impl {
    TimelineSettings settings;
    VideoNormalizer normalizer;
    std::shared_ptr<AVFrame> black;
    // CPU-only backgrounds are allowed without GPU initialization. Once GPU
    // resources exist, every access/reset must remain on the initializing worker.
    std::thread::id owner;

    void checkThread() const {
        if (owner != std::thread::id{} && owner != std::this_thread::get_id()) {
            throw std::logic_error("FrameRenderer GPU operations require its render worker");
        }
    }
};

FrameRenderer::FrameRenderer(TimelineSettings settings) : impl_(std::make_unique<Impl>()) {
    impl_->settings = std::move(settings);
}
FrameRenderer::~FrameRenderer() = default;

const TimelineSettings& FrameRenderer::settings() const {
    return impl_->settings;
}

void FrameRenderer::setSettings(TimelineSettings settings) {
    impl_->checkThread();
    if (settings == impl_->settings) return;
    shutdown();
    impl_->settings = std::move(settings);
}

bool FrameRenderer::initialize(std::string* error) {
    impl_->checkThread();
    if (isReady()) return true;
    if (!impl_->settings.valid(error)) return false;
    impl_->owner = std::this_thread::get_id();
    try {
        return impl_->normalizer.initialize(impl_->settings, error);
    } catch (const std::exception& exception) {
        impl_->normalizer.shutdown();
        if (error) *error = exception.what();
        return false;
    }
}

bool FrameRenderer::isReady() const {
    return impl_->normalizer.isReady();
}

void FrameRenderer::shutdown() {
    impl_->checkThread();
    impl_->normalizer.shutdown();
    impl_->black.reset();
    impl_->owner = {};
}

std::shared_ptr<VideoFrame> FrameRenderer::render(
    const std::shared_ptr<VideoFrame>& source, int64_t position, std::string* error) {
    impl_->checkThread();
    if (!source || !*source) return background(position, error);
    if (!initialize(error)) return {};
    auto image = impl_->normalizer.normalize(source->get(), error);
    if (!image) return {};
    // Native source timing remains in the input. Output timing is explicitly
    // timeline-local and agrees with the intended position metadata.
    image->pts = position;
    image->duration = 1;
    image->time_base = impl_->settings.timeBase().toAVRational();
    return VideoFrame::wrap(std::move(image), position, impl_->settings.frameRate());
}

std::shared_ptr<VideoFrame> FrameRenderer::background(int64_t position, std::string* error) {
    impl_->checkThread();
    if (!impl_->settings.valid(error)) return {};
    if (!impl_->black) impl_->black = makeBlack(impl_->settings, error);
    if (!impl_->black) return {};
    AVFrame* raw = av_frame_clone(impl_->black.get());
    if (!raw) {
        if (error) *error = "Cannot clone background frame";
        return {};
    }
    auto image = std::shared_ptr<AVFrame>(raw, [](AVFrame* value) { av_frame_free(&value); });
    raw->pts = position;
    raw->duration = 1;
    raw->time_base = impl_->settings.timeBase().toAVRational();
    return VideoFrame::wrap(std::move(image), position, impl_->settings.frameRate());
}

std::shared_ptr<VideoFrame> FrameRenderer::passthrough(
    const std::shared_ptr<VideoFrame>& source, int64_t position) {
    return source ? source->withPosition(position) : std::shared_ptr<VideoFrame>{};
}

} // namespace heisenberg
