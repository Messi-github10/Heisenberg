#include <Compositing/CompositionExecutor.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace heisenberg;
namespace {
CompositionTrack contribution(const std::string& id, ClipType type, const std::string& path,
                              int64_t trim = 0, double speed = 1.0) {
    CompositionClip clip;
    clip.id = id + "-clip"; clip.mediaRef = id; clip.sourcePath = path;
    clip.durationFrames = 24; clip.trimStartFrame = trim; clip.speed = speed;
    clip.volume = 0.25; clip.fadeInFrames = 2; clip.fadeOutFrames = 3;
    return {id, type, false, false, {clip}};
}
void sameRange(const AudioFrame& expected, const AudioFrame& actual) {
    ASSERT_GE(actual.position(), expected.position());
    const auto offset = static_cast<int>(actual.position() - expected.position());
    ASSERT_LE(offset + actual.samples(), expected.samples());
    for (int channel = 0; channel < actual.spec().channels; ++channel)
        EXPECT_EQ(std::memcmp(expected.channelRo(channel) + offset, actual.channelRo(channel),
            static_cast<size_t>(actual.samples()) * sizeof(float)), 0);
}
}

TEST(MultitrackExecutorTest, ValidatesIdentityAndNonoverlapWithinEachTrackOnly) {
    CompositionPlan plan; plan.frameCount = 24;
    plan.tracks = {contribution("top", ClipType::Video, ""), contribution("bottom", ClipType::Video, "")};
    CompositionExecutor executor;
    EXPECT_TRUE(executor.open(plan, {})); // Cross-track overlap is the normal case.
    auto overlap = plan.tracks[0].clips.front(); overlap.id = "overlap";
    plan.tracks[0].clips.push_back(overlap);
    EXPECT_EQ(executor.open(plan, {}).status, CompositionStatus::Failure);
    plan.tracks[0].clips.pop_back();
    plan.tracks[1].clips.front().id = plan.tracks[0].clips.front().id;
    EXPECT_EQ(executor.open(plan, {}).status, CompositionStatus::Failure);
    plan.tracks[1].clips.front().id = "unique";
    plan.tracks[1].clips.front().visualSnapshot.opacity = 1.1;
    EXPECT_EQ(executor.open(plan, {}).status, CompositionStatus::Failure);
}

#ifdef HEISENBERG_TEST_MEDIA
TEST(MultitrackExecutorTest, VideoLayersPreserveTopFirstOrderTrimOpacityHiddenAndMute) {
    CompositionPlan plan; plan.frameCount = 24;
    plan.tracks = {contribution("top", ClipType::Video, HEISENBERG_TEST_MEDIA, 2),
        contribution("hidden", ClipType::Video, HEISENBERG_TEST_MEDIA, 1),
        contribution("bottom", ClipType::Video, HEISENBERG_TEST_MEDIA)};
    plan.tracks[0].clips[0].visualSnapshot.opacity = 0.4;
    plan.tracks[0].muted = true;
    plan.tracks[1].hidden = true;
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    auto sample = executor.videoFrame(3); ASSERT_TRUE(sample) << sample.error;
    EXPECT_FALSE(sample.value.source); EXPECT_FALSE(sample.value.blank);
    ASSERT_EQ(sample.value.layers.size(), 2u);
    EXPECT_EQ(sample.value.layers[0].trackId, "top");
    EXPECT_EQ(sample.value.layers[0].clipId, "top-clip");
    EXPECT_EQ(sample.value.layers[0].clipLocalFrame, 3);
    EXPECT_DOUBLE_EQ(sample.value.layers[0].opacity, 0.4);
    EXPECT_EQ(sample.value.layers[1].trackId, "bottom");
    ASSERT_TRUE(sample.value.layers[0].frame); ASSERT_TRUE(sample.value.layers[1].frame);
    EXPECT_GT(sample.value.layers[0].frame->pts(), sample.value.layers[1].frame->pts());
    const auto owned = sample.value.plan;
    plan.tracks.clear(); executor.close();
    EXPECT_EQ(owned->tracks.size(), 3u);
    EXPECT_TRUE(sample.value.layers[0].frame);
}

TEST(MultitrackExecutorTest, AllSameFileContributionsMixAcrossChunksSeeksSpeedAndFades) {
    CompositionPlan plan; plan.frameCount = 24;
    plan.tracks = {contribution("embedded-top", ClipType::Video, HEISENBERG_TEST_MEDIA, 1, 0.75),
        contribution("embedded-hidden", ClipType::Video, HEISENBERG_TEST_MEDIA, 3, 0.6),
        contribution("audio-one", ClipType::Audio, HEISENBERG_TEST_MEDIA, 2, 1.25),
        contribution("audio-two", ClipType::Audio, HEISENBERG_TEST_MEDIA, 4, 0.5),
        contribution("muted", ClipType::Audio, "unavailable-muted-source")};
    plan.tracks[1].hidden = true; plan.tracks.back().muted = true;
    plan.tracks[2].clips[0].startFrame = 3; plan.tracks[2].clips[0].durationFrames = 17;
    auto expected = std::make_shared<AudioFrame>(); expected->setSpec(plan.settings.audioSpec());
    expected->setSamples(48000, true); expected->setPosition(0); expected->clear();
    for (size_t index = 0; index + 1 < plan.tracks.size(); ++index) {
        auto isolated = plan; isolated.tracks = {plan.tracks[index]};
        isolated.tracks[0].type = ClipType::Audio;
        CompositionExecutor reference; ASSERT_TRUE(reference.open(isolated, {}));
        auto pcm = reference.audioSamples(0, 48000); ASSERT_TRUE(pcm) << pcm.error;
        for (int channel = 0; channel < expected->spec().channels; ++channel)
            for (int sample = 0; sample < expected->samples(); ++sample)
                expected->sampleRw(channel, sample) += pcm.value->sampleRo(channel, sample);
    }
    for (int channel = 0; channel < expected->spec().channels; ++channel)
        for (int sample = 0; sample < expected->samples(); ++sample)
            expected->sampleRw(channel, sample) = std::clamp(expected->sampleRo(channel, sample), -1.0f, 1.0f);
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    for (int64_t begin = 0; begin < 48000;) {
        const auto end = std::min<int64_t>(48000, begin + 4099);
        auto actual = executor.audioSamples(begin, end); ASSERT_TRUE(actual) << actual.error;
        sameRange(*expected, *actual.value); begin = end;
    }
    for (const int64_t begin : {int64_t{22001}, int64_t{17}, int64_t{39999}, int64_t{5999}}) {
        auto video = executor.videoFrame(begin / 2000); ASSERT_TRUE(video) << video.error;
        auto actual = executor.audioSamples(begin, begin + 2001); ASSERT_TRUE(actual) << actual.error;
        sameRange(*expected, *actual.value);
    }
    auto generation = executor.audioSamples(2047, 6003, {99}); ASSERT_TRUE(generation);
    sameRange(*expected, *generation.value);
}
#endif
