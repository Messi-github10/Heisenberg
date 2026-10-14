#include <Compositing/CompositionBuilder.hpp>
#include <gtest/gtest.h>

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
