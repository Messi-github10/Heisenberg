#include <Compositing/CompositionBuilder.hpp>
#include <gtest/gtest.h>
#include <algorithm>

using namespace heisenberg;

namespace {
Timeline timelineFixture() {
    Timeline timeline;
    timeline.id = "timeline";
    timeline.fps = 30;
    Track track;
    track.id = "video";
    Clip first;
    first.id = "first"; first.mediaRef = "asset";
    first.startFrame = 3; first.durationFrames = 12;
    first.trimStartFrame = 30; first.trimEndFrame = 4; first.speed = 2.0;
    Clip second = first;
    second.id = "second"; second.startFrame = 20; second.durationFrames = 5;
    second.trimStartFrame = 90; second.speed = 0.5;
    track.clips = {second, first};
    timeline.tracks = {track};
    return timeline;
}
}

TEST(CompositionPlanTest, PureSnapshotMapsTrimSpeedCutsAndGaps) {
    auto timeline = timelineFixture();
    MediaManifest manifest;
    MediaManifestEntry entry;
    entry.id = "asset"; entry.name = "unavailable";
    entry.source.path = "deliberately-missing.mp4";
    ASSERT_TRUE(manifest.add(entry));
    CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(CompositionBuilder::build(timeline, manifest, "", plan, &error)) << error;
    EXPECT_EQ(plan.frameCount, 25);
    ASSERT_EQ(plan.clips.size(), 2);
    EXPECT_EQ(plan.clips.front().sourcePath, entry.source.path);
    EXPECT_FALSE(plan.sampleAt(0));
    EXPECT_FALSE(plan.sampleAt(15));
    EXPECT_FALSE(plan.sampleAt(25));
    auto first = plan.sampleAt(6);
    ASSERT_TRUE(first);
    EXPECT_DOUBLE_EQ(first->sourceSeconds, 1.2);
    EXPECT_DOUBLE_EQ(first->speed, 2.0);
    auto second = plan.sampleAt(22);
    ASSERT_TRUE(second);
    EXPECT_NEAR(second->sourceSeconds, 3.0 + 1.0 / 30.0, 1e-12);
    timeline.tracks.front().clips.clear();
    ASSERT_TRUE(plan.sampleAt(6));
}

TEST(CompositionPlanTest, SingleTrackFlagsAndOfflineAreValues) {
    auto timeline = timelineFixture();
    timeline.tracks.front().muted = true;
    timeline.tracks.front().hidden = true;
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan));
    ASSERT_FALSE(plan.warnings.empty());
    const auto sample = plan.sampleAt(3);
    ASSERT_TRUE(sample);
    EXPECT_TRUE(sample->muted);
    EXPECT_TRUE(sample->hidden);
    EXPECT_TRUE(plan.clips.front().sourcePath.empty());
}

TEST(CompositionPlanTest, IndependentAudioAndVideoContributionsUseBaseGainAndMaximumEnd) {
    auto timeline = timelineFixture();
    timeline.tracks.front().hidden = true;
    timeline.tracks.front().clips[1].volume = 0.8;
    timeline.tracks.front().clips[1].fadeInFrames = 6;
    Track audio;
    audio.id = "audio"; audio.type = ClipType::Audio; audio.muted = true;
    Clip clip;
    clip.id = "sound"; clip.mediaRef = "sound-asset";
    clip.mediaType = clip.sourceClipType = ClipType::Audio;
    clip.startFrame = 4; clip.durationFrames = 60;
    clip.trimStartFrame = 15; clip.speed = 0.5;
    clip.volume = 1.5; clip.fadeInFrames = 9; clip.fadeOutFrames = 12;
    audio.clips.push_back(clip);
    timeline.tracks.push_back(audio);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan));
    EXPECT_EQ(plan.videoTrackId, "video");
    EXPECT_EQ(plan.audioTrackId, "audio");
    EXPECT_EQ(plan.frameCount, 64);
    ASSERT_EQ(plan.clips.size(), 2u);
    ASSERT_EQ(plan.audioClips.size(), 1u);
    const auto videoSample = plan.videoSampleAt(6);
    const auto audioSample = plan.audioSampleAt(6);
    ASSERT_TRUE(videoSample);
    ASSERT_TRUE(audioSample);
    EXPECT_EQ(plan.sampleAt(6)->clipIndex, videoSample->clipIndex);
    EXPECT_DOUBLE_EQ(videoSample->gain, 0.8); // No frame fade baked into the gain.
    EXPECT_TRUE(videoSample->hidden);
    EXPECT_FALSE(videoSample->muted);
    EXPECT_DOUBLE_EQ(audioSample->gain, 1.5);
    EXPECT_NEAR(audioSample->sourceSeconds, 0.5 + 1.0 / 30.0, 1e-12);
    EXPECT_FALSE(audioSample->hidden);
    EXPECT_TRUE(audioSample->muted);
    EXPECT_FALSE(plan.audioSampleAt(3));
    EXPECT_FALSE(plan.videoSampleAt(30));
    EXPECT_TRUE(plan.audioSampleAt(63));
    EXPECT_FALSE(plan.audioSampleAt(64));
    timeline.tracks.back().clips.front().durationFrames = 10;
    CompositionPlan shorterAudio;
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", shorterAudio));
    EXPECT_EQ(shorterAudio.frameCount, 25); // Video can be the longer lane too.
    timeline.tracks.back().clips.clear();
    EXPECT_TRUE(plan.audioSampleAt(63)); // Both lanes are owned snapshots.
}

TEST(CompositionPlanTest, AudioOnlyPlanHasNoLegacyVideoSamples) {
    Timeline timeline;
    timeline.id = "timeline";
    Track audio;
    audio.id = "audio"; audio.type = ClipType::Audio;
    Clip clip;
    clip.id = "clip"; clip.mediaRef = "asset"; clip.durationFrames = 30;
    clip.mediaType = clip.sourceClipType = ClipType::Audio;
    audio.clips.push_back(clip);
    timeline.tracks.push_back(audio);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan));
    EXPECT_TRUE(plan.clips.empty());
    EXPECT_EQ(plan.frameCount, 30);
    EXPECT_FALSE(plan.sampleAt(0));
    EXPECT_TRUE(plan.audioSampleAt(0));
}

TEST(CompositionPlanTest, RejectsVideoExtractionAndDuplicateAudioWithoutReplacingPlan) {
    auto timeline = timelineFixture();
    Track audio;
    audio.id = "audio"; audio.type = ClipType::Audio;
    Clip clip;
    clip.id = "sound"; clip.mediaRef = "asset"; clip.mediaType = ClipType::Audio;
    audio.clips.push_back(clip); // sourceClipType remains Video: extraction is not supported.
    timeline.tracks.push_back(audio);
    CompositionPlan plan; plan.frameCount = 123;
    std::string error;
    EXPECT_EQ(compatibilityFor(timeline).level, CompatibilityLevel::Unsupported);
    EXPECT_FALSE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(plan.frameCount, 123);
    timeline.tracks.back().clips.front().sourceClipType = ClipType::Audio;
    MediaManifest mislabeledSource;
    MediaManifestEntry video;
    video.id = "asset"; video.source.path = "video.mp4"; video.type = ClipType::Video;
    ASSERT_TRUE(mislabeledSource.add(video));
    EXPECT_FALSE(CompositionBuilder::build(timeline, mislabeledSource, "", plan));
    EXPECT_EQ(plan.frameCount, 123);
    auto duplicate = audio;
    duplicate.id = "duplicate-audio"; duplicate.clips.clear();
    timeline.tracks.push_back(duplicate);
    EXPECT_FALSE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan));
    EXPECT_EQ(plan.frameCount, 123);
}

TEST(CompositionPlanTest, PreservesNonlinearAutomationAndWarnsAboutLinearFallback) {
    auto timeline = timelineFixture();
    auto& clip = timeline.tracks.front().clips.front();
    clip.volume = 1.25;
    clip.fadeInFrames = 2; clip.fadeInInterpolation = Interpolation::Smooth;
    clip.volumeTrack = KeyframeTrackPayload{JsonValue::parse(
        R"json({"keyframes":[{"frame":1,"value":0.2,"interpolationOut":"hold"}]})json")};
    const Timeline original = timeline;
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan));
    EXPECT_EQ(timeline, original);
    EXPECT_EQ(compatibilityFor(timeline).level, CompatibilityLevel::Warning);
    ASSERT_FALSE(plan.warnings.empty());
    EXPECT_DOUBLE_EQ(plan.videoSampleAt(20)->gain, 1.25);
    EXPECT_EQ(plan.clips.back().fadeInFrames, 2);
    EXPECT_TRUE(std::any_of(plan.warnings.begin(), plan.warnings.end(), [](const auto& warning) {
        return warning.find("linear fades") != std::string::npos;
    }));
}

TEST(CompositionPlanTest, RejectsMultitrackWithoutReplacingExistingPlan) {
    auto timeline = timelineFixture();
    auto track = timeline.tracks.front(); track.id = "second-track"; track.clips.clear();
    timeline.tracks.push_back(track);
    CompositionPlan plan;
    plan.frameCount = 123;
    std::string error;
    EXPECT_FALSE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(plan.frameCount, 123);
}
