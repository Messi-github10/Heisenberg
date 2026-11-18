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
    ASSERT_EQ(plan.tracks.size(), 1u);
    ASSERT_EQ(plan.tracks.front().clips.size(), 2u);
    EXPECT_EQ(plan.tracks.front().id, "video");
    EXPECT_EQ(plan.tracks.front().type, ClipType::Video);
    EXPECT_EQ(plan.tracks.front().clips.front().sourcePath, entry.source.path);
    EXPECT_FALSE(plan.sampleAt(0));
    EXPECT_FALSE(plan.sampleAt(15));
    EXPECT_FALSE(plan.sampleAt(25));
    auto first = plan.sampleAt(6);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->trackIndex, 0u);
    EXPECT_EQ(first->clipIndex, 0u);
    EXPECT_EQ(plan.tracks[first->trackIndex].clips[first->clipIndex].id, "first");
    EXPECT_DOUBLE_EQ(first->sourceSeconds, 1.2);
    EXPECT_DOUBLE_EQ(first->speed, 2.0);
    auto second = plan.sampleAt(22);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->trackIndex, 0u);
    EXPECT_EQ(second->clipIndex, 1u);
    EXPECT_EQ(plan.tracks[second->trackIndex].clips[second->clipIndex].id, "second");
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
    ASSERT_EQ(plan.tracks.size(), 1u);
    EXPECT_TRUE(plan.tracks.front().hidden);
    EXPECT_TRUE(plan.tracks.front().muted);
    ASSERT_EQ(plan.tracks.front().clips.size(), 2u);
    EXPECT_TRUE(plan.tracks.front().clips.front().sourcePath.empty());
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
    ASSERT_EQ(plan.tracks.size(), 2u);
    EXPECT_EQ(plan.tracks[0].id, "video");
    EXPECT_EQ(plan.tracks[0].type, ClipType::Video);
    EXPECT_EQ(plan.tracks[1].id, "audio");
    EXPECT_EQ(plan.tracks[1].type, ClipType::Audio);
    EXPECT_EQ(plan.frameCount, 64);
    ASSERT_EQ(plan.tracks[0].clips.size(), 2u);
    ASSERT_EQ(plan.tracks[1].clips.size(), 1u);
    const auto videoSample = plan.videoSampleAt(6);
    const auto audioSample = plan.audioSampleAt(6);
    ASSERT_TRUE(videoSample);
    ASSERT_TRUE(audioSample);
    ASSERT_TRUE(plan.sampleAt(6));
    EXPECT_EQ(plan.sampleAt(6)->trackIndex, videoSample->trackIndex);
    EXPECT_EQ(plan.sampleAt(6)->clipIndex, videoSample->clipIndex);
    EXPECT_EQ(videoSample->trackIndex, 0u);
    EXPECT_EQ(audioSample->trackIndex, 1u);
    EXPECT_EQ(plan.tracks[videoSample->trackIndex].clips[videoSample->clipIndex].id, "first");
    EXPECT_EQ(plan.tracks[audioSample->trackIndex].clips[audioSample->clipIndex].id, "sound");
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
    ASSERT_EQ(plan.tracks.size(), 1u);
    EXPECT_EQ(plan.tracks.front().type, ClipType::Audio);
    ASSERT_EQ(plan.tracks.front().clips.size(), 1u);
    EXPECT_EQ(plan.frameCount, 30);
    EXPECT_FALSE(plan.sampleAt(0));
    EXPECT_TRUE(plan.audioSampleAt(0));
}

TEST(CompositionPlanTest, RejectsVideoExtractionAndMislabeledSourceWithoutReplacingPlan) {
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
    ASSERT_EQ(plan.tracks.size(), 1u);
    ASSERT_EQ(plan.tracks.front().clips.size(), 2u);
    EXPECT_EQ(plan.tracks.front().clips.back().fadeInFrames, 2);
    EXPECT_TRUE(std::any_of(plan.warnings.begin(), plan.warnings.end(), [](const auto& warning) {
        return warning.find("linear fades") != std::string::npos;
    }));
}

TEST(CompositionPlanTest, MultipleVideoAndAudioTracksPreserveOrderFlagsAndSampleIndices) {
    auto timeline = timelineFixture();
    auto lower = timeline.tracks.front();
    lower.id = "lower-video";
    lower.hidden = true;
    lower.muted = true;
    for (auto& clip : lower.clips) clip.id = "lower-" + clip.id;
    Track audio;
    audio.id = "first-audio"; audio.type = ClipType::Audio; audio.hidden = true;
    Clip sound;
    sound.id = "first-sound"; sound.mediaRef = "sound";
    sound.mediaType = sound.sourceClipType = ClipType::Audio;
    sound.startFrame = 4; sound.durationFrames = 30;
    sound.trimStartFrame = 15; sound.volume = 0.5;
    audio.clips.push_back(sound);
    auto secondAudio = audio;
    secondAudio.id = "second-audio"; secondAudio.hidden = false; secondAudio.muted = true;
    secondAudio.clips.front().id = "second-sound";
    secondAudio.clips.front().volume = 1.5;
    // Interleave types to prove canonical UI order and nonzero video indices.
    timeline.tracks = {audio, timeline.tracks.front(), secondAudio, lower};
    CompositionPlan plan;
    std::string error;
    EXPECT_EQ(compatibilityFor(timeline).level, CompatibilityLevel::Supported);
    ASSERT_TRUE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan, &error)) << error;
    EXPECT_EQ(plan.frameCount, 34);
    ASSERT_EQ(plan.tracks.size(), 4u);
    for (size_t i = 0; i < plan.tracks.size(); ++i) {
        EXPECT_EQ(plan.tracks[i].id, timeline.tracks[i].id);
        EXPECT_EQ(plan.tracks[i].type, timeline.tracks[i].type);
        EXPECT_EQ(plan.tracks[i].hidden, timeline.tracks[i].hidden);
        EXPECT_EQ(plan.tracks[i].muted, timeline.tracks[i].muted);
        EXPECT_EQ(plan.tracks[i].clips.size(), timeline.tracks[i].clips.size());
    }
    const auto videos = plan.videoSamplesAt(6);
    ASSERT_EQ(videos.size(), 2u);
    EXPECT_EQ(videos[0].trackIndex, 1u); EXPECT_EQ(videos[1].trackIndex, 3u);
    for (const auto& sample : videos) {
        EXPECT_EQ(sample.clipIndex, 0u);
        EXPECT_EQ(sample.trackId, plan.tracks[sample.trackIndex].id);
        EXPECT_EQ(sample.clipLocalFrame, 3);
        EXPECT_DOUBLE_EQ(sample.sourceSeconds, 1.2);
        EXPECT_EQ(sample.hidden, plan.tracks[sample.trackIndex].hidden);
        EXPECT_EQ(sample.muted, plan.tracks[sample.trackIndex].muted);
    }
    EXPECT_EQ(plan.tracks[videos[0].trackIndex].clips[videos[0].clipIndex].id, "first");
    EXPECT_EQ(plan.tracks[videos[1].trackIndex].clips[videos[1].clipIndex].id, "lower-first");
    EXPECT_FALSE(videos[0].hidden); EXPECT_FALSE(videos[0].muted);
    EXPECT_TRUE(videos[1].hidden); EXPECT_TRUE(videos[1].muted);
    const auto sounds = plan.audioSamplesAt(6); // Includes embedded audio on video tracks.
    ASSERT_EQ(sounds.size(), 4u);
    for (size_t i = 0; i < sounds.size(); ++i) {
        EXPECT_EQ(sounds[i].trackIndex, i);
        EXPECT_EQ(sounds[i].trackId, plan.tracks[i].id);
        EXPECT_EQ(sounds[i].clipIndex, 0u);
        EXPECT_EQ(sounds[i].hidden, plan.tracks[i].hidden);
        EXPECT_EQ(sounds[i].muted, plan.tracks[i].muted);
    }
    EXPECT_EQ(plan.tracks[sounds[0].trackIndex].clips[sounds[0].clipIndex].id, "first-sound");
    EXPECT_EQ(plan.tracks[sounds[2].trackIndex].clips[sounds[2].clipIndex].id, "second-sound");
    EXPECT_DOUBLE_EQ(sounds[0].gain, 0.5); EXPECT_DOUBLE_EQ(sounds[2].gain, 1.5);
    EXPECT_NEAR(sounds[0].sourceSeconds, 17.0 / 30.0, 1e-12);
    EXPECT_NEAR(sounds[2].sourceSeconds, 17.0 / 30.0, 1e-12);
    ASSERT_TRUE(plan.videoSampleAt(6)); EXPECT_EQ(plan.videoSampleAt(6)->trackIndex, 1u);
    ASSERT_TRUE(plan.audioSampleAt(6)); EXPECT_EQ(plan.audioSampleAt(6)->trackIndex, 0u);
    const auto secondClips = plan.videoSamplesAt(22);
    ASSERT_EQ(secondClips.size(), 2u);
    for (const auto& sample : secondClips) {
        EXPECT_EQ(sample.clipIndex, 1u);
        EXPECT_EQ(sample.clipLocalFrame, 2);
        EXPECT_NEAR(sample.sourceSeconds, 3.0 + 1.0 / 30.0, 1e-12);
    }
    EXPECT_TRUE(plan.videoSamplesAt(15).empty());
    EXPECT_TRUE(plan.videoSamplesAt(25).empty());
    const auto tail = plan.audioSamplesAt(33);
    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0].trackIndex, 0u); EXPECT_EQ(tail[1].trackIndex, 2u);
    EXPECT_TRUE(plan.audioSamplesAt(34).empty());
    timeline.tracks.clear();
    EXPECT_EQ(plan.videoSamplesAt(6).size(), 2u);
    EXPECT_EQ(plan.audioSamplesAt(6).size(), 4u);
}

TEST(CompositionPlanTest, RejectsUnsupportedTrackTypesWithoutReplacingExistingPlan) {
    for (const auto type : {ClipType::Image, ClipType::Text, ClipType::Lottie, ClipType::Sequence, ClipType::Subtitle}) {
        auto timeline = timelineFixture();
        Track unsupported; unsupported.id = "unsupported"; unsupported.type = type;
        timeline.tracks.push_back(unsupported);
        CompositionPlan plan; plan.frameCount = 123;
        std::string error;
        EXPECT_EQ(compatibilityFor(timeline).level, CompatibilityLevel::Unsupported);
        EXPECT_FALSE(CompositionBuilder::build(timeline, MediaManifest{}, "", plan, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(plan.frameCount, 123);
    }
}
