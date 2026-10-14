#include <Models/TimelineSettings.hpp>
#include <Common/MediaMath.hpp>
#include <Common/MediaFrame.hpp>
#include <Common/VideoFrame.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using heisenberg::Rational;
using heisenberg::TimelineSettings;

TEST(TimelineSettingsTest, BuiltInPresetsHaveValidatedVideoAndAudioSpecs) {
    const auto presets = TimelineSettings::presets();
    ASSERT_GE(presets.size(), 12u);
    for (const auto& preset : presets) {
        std::string error;
        EXPECT_TRUE(preset.valid(&error)) << preset.id() << ": " << error;
        ASSERT_FALSE(preset.id().empty());
        const auto* found = TimelineSettings::findPreset(preset.id());
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(*found, preset);
        EXPECT_EQ(preset.videoSpec().width, preset.width());
        EXPECT_EQ(preset.audioSpec().sampleRate, preset.sampleRate());
    }
}

TEST(TimelineSettingsTest, FractionalFrameRateProducesExactSampleTotals) {
    const auto hd24 = TimelineSettings::hd1080p24();
    EXPECT_EQ(hd24.audioSamplesForFrame(0), 2000);
    const auto* ntsc = TimelineSettings::findPreset("hd_1080p_2997");
    ASSERT_NE(ntsc, nullptr);
    EXPECT_EQ(ntsc->frameRate(), Rational(30000, 1001));
    EXPECT_EQ(ntsc->audioSamplesForFrame(0) + ntsc->audioSamplesForFrame(1), 3203);
    EXPECT_EQ(ntsc->audioSamplePosition(30000), 48000LL * 1001);
    EXPECT_EQ(ntsc->frameFromSeconds(ntsc->secondsFromFrame(1000)), 1000);
    int64_t accumulated = 0;
    for (int64_t index = 0; index < 30000; ++index) accumulated += ntsc->audioSamplesForFrame(index);
    EXPECT_EQ(accumulated, ntsc->audioSamplePosition(30000));
    EXPECT_EQ(ntsc->audioSamplePosition(1), heisenberg::samplesToPosition(ntsc->frameRate(), ntsc->sampleRate(), 1));
    EXPECT_EQ(ntsc->audioSamplePosition(-1), heisenberg::samplesToPosition(ntsc->frameRate(), ntsc->sampleRate(), -1));
}

TEST(TimelineSettingsTest, InvalidDimensionsAndRatesAreRejected) {
    auto video = TimelineSettings::hd1080p24().videoSpec();
    video.width = 0;
    EXPECT_FALSE(TimelineSettings(video).valid());
    video.width = TimelineSettings::kMaxDimension + 1;
    EXPECT_FALSE(TimelineSettings(video).valid());
    video.width = 1920;
    video.frameRate = Rational(0, 1);
    EXPECT_FALSE(TimelineSettings(video).valid());
    video.frameRate = Rational(24, 1);
    auto audio = heisenberg::AudioSpec{};
    audio.channels = 0;
    EXPECT_FALSE(TimelineSettings(video, audio).valid());
}

TEST(RationalTest, ConversionKeepsNumeratorDenominatorOrder) {
    EXPECT_EQ(Rational::fromDouble(30.0), Rational(30, 1));
    EXPECT_EQ(Rational::fromDouble(0.5), Rational(1, 2));
    EXPECT_EQ(Rational::fromDouble(-2.5), Rational(-5, 2));
    EXPECT_TRUE(Rational(0, 0).isNaN());
}

TEST(AudioFrameTest, NegativePrerollIsAValidPositionUntilReset) {
    heisenberg::AudioFrame frame;
    EXPECT_FALSE(frame.hasPosition());
    frame.setPosition(-1);
    EXPECT_TRUE(frame.hasPosition());
    EXPECT_EQ(frame.position(), -1);
    frame.setPosition(-1024);
    const auto copy = frame;
    EXPECT_TRUE(copy.hasPosition());
    EXPECT_EQ(copy.position(), -1024);
    frame.reset();
    EXPECT_FALSE(frame.hasPosition());
}

TEST(VideoFrameTest, MetadataOwnershipAndOutputPositionAreSeparate) {
    auto native = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });
    native->format = AV_PIX_FMT_YUV420P;
    native->width = 32;
    native->height = 24;
    native->pts = 9000;
    native->duration = 3000;
    native->time_base = {1, 90000};
    native->color_primaries = AVCOL_PRI_BT2020;
    native->color_trc = AVCOL_TRC_SMPTE2084;
    native->colorspace = AVCOL_SPC_BT2020_NCL;
    ASSERT_EQ(av_frame_get_buffer(native.get(), 32), 0);
    auto frame = heisenberg::VideoFrame::wrap(native, 12, Rational(30, 1));
    auto moved = frame->withPosition(15);
    auto cloned = frame->clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(frame->position(), 12);
    EXPECT_EQ(moved->position(), 15);
    EXPECT_EQ(moved->native(), native);
    EXPECT_NE(cloned->get(), native.get());
    EXPECT_EQ(frame->pts(), 9000);
    EXPECT_EQ(frame->timeBase(), Rational(1, 90000));
    EXPECT_EQ(frame->spec().colorPrimaries, AVCOL_PRI_BT2020);
    EXPECT_EQ(native->pts, 9000);
    const auto message = heisenberg::MediaFrame::video(frame, 7);
    EXPECT_EQ(message.videoFrame(), frame);
    EXPECT_EQ(message.metadata.generation, 7u);
    native.reset();
    EXPECT_EQ(frame->get()->width, 32);
}
