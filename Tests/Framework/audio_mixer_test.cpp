#include <MultiMedia/Audio/AudioMixer.hpp>
#include <MultiMedia/Audio/AudioResampler.hpp>
#include <gtest/gtest.h>

using namespace heisenberg;

namespace {
AudioSpec stereo() {
    AudioSpec spec; spec.sampleRate = 48000; spec.channels = 2;
    spec.layout = defaultLayout(2); return spec;
}
std::shared_ptr<AudioFrame> ramp() {
    auto frame = std::make_shared<AudioFrame>();
    frame->setSpec(stereo()); frame->setPosition(100); frame->setSamples(8, true);
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 8; ++s) frame->sampleRw(c, s) = s * 0.1f;
    return frame;
}
}

TEST(AudioMixerTest, MapsTrimSpeedAndTargetTiming) {
    auto source = ramp();
    auto output = AudioMixer::render(stereo(), 3, 4000, {source}, 100.0 / 48000.0, 2.0, 0.5);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->position(), 4000);
    EXPECT_EQ(output->samples(), 3);
    EXPECT_NEAR(output->sampleRo(0, 0), 0.0, 1e-6);
    EXPECT_NEAR(output->sampleRo(0, 1), 0.1, 1e-6);
    EXPECT_NEAR(output->sampleRo(1, 2), 0.2, 1e-6);
    EXPECT_EQ(source->position(), 100);
}

TEST(AudioMixerTest, FractionalSpeedInterpolatesAndGapsStaySilent) {
    auto output = AudioMixer::render(stereo(), 4, 0, {ramp()}, 100.0 / 48000.0, 0.5);
    ASSERT_TRUE(output);
    EXPECT_NEAR(output->sampleRo(0, 1), 0.05, 1e-6);
    EXPECT_NEAR(output->sampleRo(0, 3), 0.15, 1e-6);
    auto gap = AudioMixer::render(stereo(), 4, 10, {}, 0.0);
    auto mute = AudioMixer::render(stereo(), 4, 10, {ramp()}, 100.0 / 48000.0, 1.0, 1.0, true);
    for (int c = 0; c < 2; ++c) for (int s = 0; s < 4; ++s) {
        EXPECT_EQ(gap->sampleRo(c, s), 0.0f);
        EXPECT_EQ(mute->sampleRo(c, s), 0.0f);
    }
}

TEST(AudioResamplerTest, ConversionRetainsSampleTimestampAndResets) {
    AudioResampler resampler;
    auto input = ramp();
    auto converted = resampler.convert(*input, stereo());
    ASSERT_TRUE(converted);
    EXPECT_EQ(converted->position(), input->position());
    EXPECT_EQ(converted->spec().sampleRate, 48000);
    EXPECT_EQ(converted->spec().channels, 2);
    EXPECT_EQ(converted->samples(), 8);
    EXPECT_NEAR(converted->sampleRo(1, 6), 0.6, 1e-6);
    resampler.reset();
    ASSERT_TRUE(resampler.convert(*input, stereo()));
}
