#include <Compositing/FrameRenderer.hpp>
#include <Models/TimelineSettings.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace {

void expectBlack(const std::shared_ptr<heisenberg::VideoFrame>& frame, int width, int height) {
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(frame->get(), nullptr);
    EXPECT_EQ(frame->spec().width, width);
    EXPECT_EQ(frame->spec().height, height);
    const auto* image = frame->get();
    ASSERT_NE(image->data[0], nullptr);
    if (image->format == AV_PIX_FMT_RGBA) {
        EXPECT_EQ(image->data[0][0], 0);
        EXPECT_EQ(image->data[0][1], 0);
        EXPECT_EQ(image->data[0][2], 0);
        EXPECT_EQ(image->data[0][3], 255);
    } else if (image->format == AV_PIX_FMT_RGBAF16) {
        const auto* pixel = reinterpret_cast<const uint16_t*>(image->data[0]);
        EXPECT_EQ(pixel[0], 0);
        EXPECT_EQ(pixel[1], 0);
        EXPECT_EQ(pixel[2], 0);
        EXPECT_EQ(pixel[3], 0x3c00);
    } else if (image->format == AV_PIX_FMT_RGBAF32) {
        const auto* pixel = reinterpret_cast<const float*>(image->data[0]);
        EXPECT_FLOAT_EQ(pixel[0], 0.0f);
        EXPECT_FLOAT_EQ(pixel[1], 0.0f);
        EXPECT_FLOAT_EQ(pixel[2], 0.0f);
        EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    } else {
        FAIL() << "unexpected canvas format " << image->format;
    }
}

} // namespace

TEST(FrameRendererTest, BackgroundHasOutputSpecAndIndependentTimestamp) {
    const auto settings = heisenberg::TimelineSettings::fromTimelineSettings(320, 240, 24);
    heisenberg::FrameRenderer renderer(settings);
    std::string error;
    const auto first = renderer.background(0, &error);
    const auto last = renderer.background(9, &error);
    expectBlack(first, 320, 240);
    expectBlack(last, 320, 240);
    ASSERT_NE(first, nullptr) << error;
    ASSERT_NE(last, nullptr) << error;
    EXPECT_EQ(first->position(), 0);
    EXPECT_EQ(last->position(), 9);
    EXPECT_EQ(first->pts(), 0);
    EXPECT_EQ(last->pts(), 9);
    EXPECT_EQ(first->timeBase(), heisenberg::Rational(1, 24));
    EXPECT_NE(first->get(), last->get());
}

TEST(FrameRendererTest, BackgroundSupportsEveryWorkingPixelFormat) {
    for (const auto format : {heisenberg::WorkingFormat::Rgba8,
                             heisenberg::WorkingFormat::Rgba16f,
                             heisenberg::WorkingFormat::Rgba32f}) {
        auto settings = heisenberg::TimelineSettings::fromTimelineSettings(64, 48, 30);
        settings.setWorkingFormat(format);
        heisenberg::FrameRenderer renderer(settings);
        std::string error;
        expectBlack(renderer.background(1, &error), 64, 48);
    }
}
