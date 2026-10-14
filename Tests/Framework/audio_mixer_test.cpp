#include <MultiMedia/Audio/AudioMixer.hpp>
#include <MultiMedia/Audio/AudioResampler.hpp>
#include <gtest/gtest.h>
#include <limits>

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

TEST(AudioMixerTest, IndependentInputsSumThenClampForMonoAndStereo) {
    for (int channels : {1, 2}) {
        AudioSpec spec = stereo(); spec.channels = channels; spec.layout = defaultLayout(channels);
        auto make = [&](float value) {
            auto frame = std::make_shared<AudioFrame>();
            frame->setSpec(spec); frame->setPosition(0); frame->setSamples(4, true);
            frame->assign(static_cast<size_t>(4 * channels), value);
            return frame;
        };
        std::vector<std::shared_ptr<AudioFrame>> first{make(0.8f)};
        std::vector<std::shared_ptr<AudioFrame>> second{make(0.7f)};
        std::vector<std::shared_ptr<AudioFrame>> inverse{make(-0.9f)};
        std::vector<AudioMixer::Input> inputs{{&first}, {&second}, {&inverse}};
        const auto cancellation = AudioMixer::render(spec, 3, 10, inputs);
        ASSERT_TRUE(cancellation);
        EXPECT_EQ(cancellation->position(), 10);
        for (int c = 0; c < channels; ++c)
            for (int s = 0; s < 3; ++s) EXPECT_NEAR(cancellation->sampleRo(c, s), 0.6f, 1e-6);
        inputs.pop_back();
        const auto clipped = AudioMixer::render(spec, 3, 0, inputs);
        inputs = {{&inverse, 0.0, 1.0, 2.0}};
        const auto negativeClipped = AudioMixer::render(spec, 3, 0, inputs);
        for (int c = 0; c < channels; ++c) for (int s = 0; s < 3; ++s) {
            EXPECT_FLOAT_EQ(clipped->sampleRo(c, s), 1.0f);
            EXPECT_FLOAT_EQ(negativeClipped->sampleRo(c, s), -1.0f);
        }
        inputs = {{&first}, {&second, 0.0, 1.0, 1.0, true}};
        const auto independentMute = AudioMixer::render(spec, 3, 0, inputs);
        EXPECT_NEAR(independentMute->sampleRo(0, 1), 0.8f, 1e-6);
    }
}

TEST(AudioMixerTest, SampleFadesUseTimelineTimeOnceAndGateClipBoundaries) {
    for (int channels : {1, 2}) {
        AudioSpec spec; spec.sampleRate = 8; spec.channels = channels; spec.layout = defaultLayout(channels);
        auto frame = std::make_shared<AudioFrame>();
        frame->setSpec(spec); frame->setPosition(0); frame->setSamples(20, true);
        frame->assign(static_cast<size_t>(20 * channels), 0.5f);
        std::vector<std::shared_ptr<AudioFrame>> source{frame};
        AudioMixer::Input input{&source};
        input.gain = 1.5; input.speed = 2.0;
        input.clipDurationSeconds = 1.0;
        input.fadeInSeconds = 0.5; input.fadeOutSeconds = 0.5;
        const auto full = AudioMixer::render(spec, 10, 50, std::vector<AudioMixer::Input>{input});
        ASSERT_TRUE(full);
        const float expected[] = {0.0f, 0.1875f, 0.375f, 0.5625f, 0.75f, 0.5625f, 0.375f, 0.1875f, 0.0f, 0.0f};
        for (int c = 0; c < channels; ++c)
            for (int s = 0; s < 10; ++s) EXPECT_NEAR(full->sampleRo(c, s), expected[s], 1e-6);
        input.clipOffsetSeconds = 0.25; input.sourceSeconds = 0.5;
        const auto part = AudioMixer::render(spec, 4, 52, std::vector<AudioMixer::Input>{input});
        for (int s = 0; s < 4; ++s) EXPECT_NEAR(part->sampleRo(0, s), expected[s + 2], 1e-6);
        input.fadeInSeconds = 0.0; input.fadeOutSeconds = 0.0;
        input.clipOffsetSeconds = -0.125; input.sourceSeconds = 0.0;
        const auto before = AudioMixer::render(spec, 2, 0, std::vector<AudioMixer::Input>{input});
        EXPECT_FLOAT_EQ(before->sampleRo(0, 0), 0.0f);
        EXPECT_FLOAT_EQ(before->sampleRo(0, 1), 0.75f);
        input.clipOffsetSeconds = 0.875;
        const auto after = AudioMixer::render(spec, 3, 0, std::vector<AudioMixer::Input>{input});
        EXPECT_FLOAT_EQ(after->sampleRo(0, 0), 0.75f);
        EXPECT_FLOAT_EQ(after->sampleRo(0, 1), 0.0f);
        EXPECT_FLOAT_EQ(after->sampleRo(0, 2), 0.0f);
    }
}

TEST(AudioMixerTest, InterpolatesAcrossPositionedFramesAndKeepsChannelsIndependent) {
    for (int channels : {1, 2}) {
        auto spec = stereo(); spec.channels = channels; spec.layout = defaultLayout(channels);
        auto first = std::make_shared<AudioFrame>();
        auto second = std::make_shared<AudioFrame>();
        first->setSpec(spec); second->setSpec(spec);
        first->setPosition(-2); second->setPosition(0);
        first->setSamples(2, true); second->setSamples(2, true);
        for (int c = 0; c < channels; ++c) {
            const float sign = c == 0 ? 1.0f : -1.0f;
            first->sampleRw(c, 0) = 0.0f;
            first->sampleRw(c, 1) = sign * 0.2f;
            second->sampleRw(c, 0) = sign * 0.4f;
            second->sampleRw(c, 1) = sign * 0.6f;
        }
        const std::vector<std::shared_ptr<AudioFrame>> source{first, second};
        const auto output = AudioMixer::render(spec, 4, 0, source, -1.5 / spec.sampleRate, 0.5);
        for (int c = 0; c < channels; ++c) {
            const float sign = c == 0 ? 1.0f : -1.0f;
            for (int s = 0; s < 4; ++s) EXPECT_NEAR(output->sampleRo(c, s), sign * (0.1f + 0.1f * s), 1e-6);
        }
    }
}

TEST(AudioMixerTest, InvalidMappingsAndMismatchedOrUnpositionedFramesAreSilent) {
    const auto spec = stereo();
    auto frame = ramp();
    std::vector<std::shared_ptr<AudioFrame>> source{frame};
    for (double speed : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
        const auto output = AudioMixer::render(spec, 2, 0, source, 0.0, speed);
        ASSERT_TRUE(output);
        EXPECT_FLOAT_EQ(output->sampleRo(0, 0), 0.0f);
    }
    const auto extreme = AudioMixer::render(spec, 2, 0, source, std::numeric_limits<double>::max());
    EXPECT_FLOAT_EQ(extreme->sampleRo(0, 0), 0.0f);
    auto wrongSpec = spec; wrongSpec.sampleRate = 44100;
    frame->setSpec(wrongSpec);
    const auto mismatch = AudioMixer::render(spec, 2, 0, source, 102.0 / spec.sampleRate);
    EXPECT_FLOAT_EQ(mismatch->sampleRo(0, 0), 0.0f);
    auto unpositioned = std::make_shared<AudioFrame>();
    unpositioned->setSpec(spec); unpositioned->setSamples(4, true); unpositioned->assign(8, 0.5f);
    const auto unknown = AudioMixer::render(spec, 2, 0, {unpositioned}, 0.0);
    EXPECT_FLOAT_EQ(unknown->sampleRo(0, 0), 0.0f);
    auto invalid = spec; invalid.sampleRate = 0;
    EXPECT_FALSE(AudioMixer::render(invalid, 1, 0, std::vector<AudioMixer::Input>{}));
    EXPECT_FALSE(AudioMixer::render(spec, -1, 0, std::vector<AudioMixer::Input>{}));
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
