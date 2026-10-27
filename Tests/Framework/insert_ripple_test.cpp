#include <Compositing/Grade/GradeCompiler.hpp>
#include <Editor/EditorViewModel.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace heisenberg;

Clip makeClip(const std::string& id, int64_t start, int64_t duration,
              ClipType type = ClipType::Video) {
    Clip clip;
    clip.id = id;
    clip.mediaRef = type == ClipType::Audio ? "audio-media" : "video-media";
    clip.mediaType = clip.sourceClipType = type;
    clip.startFrame = start;
    clip.durationFrames = duration;
    return clip;
}

Clip animatedClip(const std::string& id, int64_t start, ClipType type = ClipType::Video) {
    Clip clip = makeClip(id, start, 24, type);
    clip.speed = 2.0;
    clip.trimStartFrame = 3;
    clip.trimEndFrame = 7;
    clip.volume = 0.6;
    clip.fadeInFrames = 4;
    clip.fadeOutFrames = 5;
    clip.fadeInInterpolation = Interpolation::Smooth;
    clip.fadeOutInterpolation = Interpolation::Hold;
    clip.linkGroupId = "linked-but-not-rippled";
    clip.opacityTrack = KeyframeTrackPayload{JsonValue::parse(
        R"({"keyframes":[{"frame":0,"value":0.2},{"frame":12,"value":0.8},{"frame":30,"value":1.0}],"opaque":"keep"})")};
    clip.positionTrack = clip.scaleTrack = clip.rotationTrack = clip.cropTrack = clip.opacityTrack;
    clip.volumeTrack = clip.opacityTrack;
    clip.wordTimings = std::vector<WordTiming>{{"cross", 5, 10}, {"last", 20, 23}};
    if (type == ClipType::Video) {
        clip.effects = JsonValue::parse(
            R"([{"id":"basic-grade","type":"video.basicGrade","enabled":true,"params":{"exposure":{"value":1,"track":{"keyframes":[{"frame":0,"value":1,"interpolationOut":"linear"},{"frame":12,"value":3,"interpolationOut":"linear"},{"frame":30,"value":2,"interpolationOut":"hold"}]}}},"opaque":"preserve"}])");
    }
    return clip;
}

MediaManifest makeManifest() {
    MediaManifest manifest;
    for (const auto type : {ClipType::Video, ClipType::Audio, ClipType::Image}) {
        MediaManifestEntry entry;
        entry.id = type == ClipType::Video ? "video-media" : type == ClipType::Audio ? "audio-media" : "image-media";
        entry.type = type;
        entry.source = {MediaSource::Kind::External, "C:/offline-insert-ripple.media"};
        entry.duration = 10.0; // 300 source frames, without importing/decoding any media.
        EXPECT_TRUE(manifest.add(entry));
    }
    MediaManifestEntry huge;
    huge.id = "huge-media";
    huge.source = {MediaSource::Kind::External, "C:/offline-huge.mp4"};
    huge.duration = 1e18;
    EXPECT_TRUE(manifest.add(huge));
    return manifest;
}

ProjectFile makeProject(std::vector<Clip> video, std::vector<Clip> audio = {}) {
    ProjectFile project;
    Timeline timeline;
    timeline.id = "timeline";
    timeline.fps = 30;
    Track videoTrack;
    videoTrack.id = "video";
    videoTrack.type = ClipType::Video;
    videoTrack.clips = std::move(video);
    Track audioTrack;
    audioTrack.id = "audio";
    audioTrack.type = ClipType::Audio;
    audioTrack.clips = std::move(audio);
    timeline.tracks = {std::move(videoTrack), std::move(audioTrack)};
    project.timelines.push_back(std::move(timeline));
    project.activeTimelineId = "timeline";
    return project;
}

void expectSingleHistoryStep(EditorViewModel& editor, const ProjectFile& before,
                             ProjectFile after) {
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after); // Includes generated track, split and inserted IDs.
    EXPECT_FALSE(editor.redo());
}

float exposureAt(const Clip& clip, int64_t localFrame) {
    grade::CompiledGrade compiled;
    std::string error;
    EXPECT_TRUE(grade::GradeCompiler::compile(clip, localFrame, compiled, &error)) << error;
    if (compiled.stages.empty()) return 0.0f;
    return std::get<float>(compiled.stages[0].parameterUpdates[0].parameters.at("exposure"));
}
} // namespace

TEST(InsertRippleTest, InteriorInsertReusesSplitPayloadSemanticsAndCommitsOnce) {
    const auto source = animatedClip("crossing", 10);
    const auto later = animatedClip("later", 50);
    auto audio = animatedClip("audio-untouched", 0, ClipType::Audio);
    const auto before = makeProject({makeClip("before", 0, 5), source, later}, {audio});
    EditorViewModel editor, splitReference;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    ASSERT_TRUE(splitReference.replaceDocument(before, makeManifest()));
    const auto reference = splitReference.splitClip(source.id, 17);
    ASSERT_TRUE(reference.changed());
    const auto revision = editor.revision();
    const auto inserted = editor.insertClip("video-media", 17, 5, 11, "video");
    ASSERT_TRUE(inserted.changed()) << inserted.message;
    ASSERT_EQ(inserted.affectedIds.size(), 4u);
    const auto* track = editor.activeTimeline()->trackById("video");
    ASSERT_EQ(track->clips.size(), 5u);
    EXPECT_EQ(inserted.affectedIds.front(), source.id);
    EXPECT_EQ(*track->clipById(source.id), *splitReference.activeTimeline()->clipById(source.id));
    Clip expectedRight = *splitReference.activeTimeline()->clipById(reference.affectedIds.back());
    expectedRight.id = inserted.affectedIds[1];
    expectedRight.startFrame = 22;
    ASSERT_NE(track->clipById(expectedRight.id), nullptr);
    EXPECT_EQ(*track->clipById(expectedRight.id), expectedRight);
    EXPECT_EQ(expectedRight.trimStartFrame, 17);
    EXPECT_EQ(expectedRight.trimEndFrame, 7);
    EXPECT_EQ(track->clipById(source.id)->trimEndFrame, 41);
    EXPECT_EQ(track->clipById(source.id)->fadeOutFrames, 0);
    EXPECT_EQ(expectedRight.fadeInFrames, 0);
    EXPECT_EQ(expectedRight.fadeOutFrames, 5);
    EXPECT_EQ(*expectedRight.wordTimings,
              (std::vector<WordTiming>{{"cross", 0, 3}, {"last", 13, 16}}));
    Clip expectedLater = later;
    expectedLater.startFrame += 5;
    EXPECT_EQ(*track->clipById(later.id), expectedLater); // Local payloads must not shift.
    EXPECT_EQ(*track->clipById("before"), before.timelines[0].tracks[0].clips[0]);
    EXPECT_EQ(*editor.activeTimeline()->trackById("audio"), before.timelines[0].tracks[1]);
    const auto* added = track->clipById(inserted.affectedIds.back());
    ASSERT_NE(added, nullptr); // affectedIds.back() remains the inserted-ID convention.
    EXPECT_EQ(added->mediaRef, "video-media");
    EXPECT_EQ(added->startFrame, 17);
    EXPECT_EQ(added->durationFrames, 5);
    EXPECT_EQ(added->trimStartFrame, 11);
    EXPECT_EQ(added->trimEndFrame, 284);
    EXPECT_EQ(editor.revision(), revision + 1);
    expectSingleHistoryStep(editor, before, editor.project());
}

TEST(InsertRippleTest, InteriorInsertPreservesHoldAndLinearGradeSamplesInClipLocalTime) {
    for (const std::string interpolation : {"hold", "linear"}) {
        SCOPED_TRACE(interpolation);
        auto original = animatedClip("animated", 10);
        auto& keys = (*original.effects)[0]["params"]["exposure"]["track"]["keyframes"];
        keys[0]["interpolationOut"] = interpolation;
        keys[1]["interpolationOut"] = interpolation;
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(makeProject({original}), makeManifest()));
        const auto result = editor.insertClip("video-media", 17, 5);
        ASSERT_TRUE(result.changed()) << result.message;
        const auto& clips = editor.activeTimeline()->trackById("video")->clips;
        ASSERT_EQ(clips.size(), 3u);
        const auto& left = clips[0];
        const auto& right = clips[2];
        for (int64_t frame = 0; frame < left.durationFrames; ++frame)
            EXPECT_FLOAT_EQ(exposureAt(left, frame), exposureAt(original, frame)) << frame;
        for (int64_t frame = 0; frame < right.durationFrames; ++frame)
            EXPECT_FLOAT_EQ(exposureAt(right, frame), exposureAt(original, frame + 7)) << frame;
        EXPECT_EQ((*right.effects)[0]["params"]["exposure"]["track"]["keyframes"][0]["frame"], -7);
        EXPECT_EQ((*right.effects)[0]["opaque"], "preserve");
    }
}

TEST(InsertRippleTest, InsertAtBoundariesInGapsAndAfterTailShiftsOnlyStartsAtOrAfterPoint) {
    for (const int64_t point : {0, 10, 20, 25, 30, 40, 45}) {
        SCOPED_TRACE(point);
        const auto before = makeProject({makeClip("first", 10, 10), makeClip("second", 30, 10)},
                                        {makeClip("other-track", 0, 80, ClipType::Audio)});
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
        const auto result = editor.insertClip("video-media", point, 5);
        ASSERT_TRUE(result.changed()) << result.message;
        const auto* track = editor.activeTimeline()->trackById("video");
        ASSERT_EQ(track->clips.size(), 3u); // A boundary must never create a split.
        std::vector<std::string> expectedAffected;
        for (auto expected : before.timelines[0].tracks[0].clips) {
            if (expected.startFrame >= point) {
                expected.startFrame += 5;
                expectedAffected.push_back(expected.id);
            }
            ASSERT_NE(track->clipById(expected.id), nullptr);
            EXPECT_EQ(*track->clipById(expected.id), expected);
        }
        ASSERT_FALSE(result.affectedIds.empty());
        expectedAffected.push_back(result.affectedIds.back());
        EXPECT_EQ(result.affectedIds, expectedAffected);
        const auto* added = track->clipById(result.affectedIds.back());
        ASSERT_NE(added, nullptr);
        EXPECT_EQ(added->startFrame, point);
        EXPECT_EQ(added->durationFrames, 5);
        EXPECT_EQ(*editor.activeTimeline()->trackById("audio"), before.timelines[0].tracks[1]);
        expectSingleHistoryStep(editor, before, editor.project());
    }
}

TEST(InsertRippleTest, AudioInsertSplitsSourceTrimsFadesAndAutomationWithoutChangingVideo) {
    const auto original = animatedClip("audio-source", 10, ClipType::Audio);
    const auto before = makeProject({animatedClip("video-untouched", 0)},
                                    {original, makeClip("audio-later", 50, 10, ClipType::Audio)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    const auto result = editor.insertClip("audio-media", 17, 5, 9, "audio");
    ASSERT_TRUE(result.changed()) << result.message;
    const auto* track = editor.activeTimeline()->trackById("audio");
    ASSERT_EQ(track->clips.size(), 4u);
    const auto& left = track->clips[0];
    const auto& right = track->clips[2];
    EXPECT_EQ(left.id, original.id);
    EXPECT_EQ(left.durationFrames, 7);
    EXPECT_EQ(left.trimStartFrame, 3);
    EXPECT_EQ(left.trimEndFrame, 41);
    EXPECT_EQ(left.fadeInFrames, 4);
    EXPECT_EQ(left.fadeOutFrames, 0);
    EXPECT_EQ(right.startFrame, 22);
    EXPECT_EQ(right.durationFrames, 17);
    EXPECT_EQ(right.trimStartFrame, 17);
    EXPECT_EQ(right.trimEndFrame, 7);
    EXPECT_EQ(right.fadeInFrames, 0);
    EXPECT_EQ(right.fadeOutFrames, 5);
    EXPECT_EQ(right.fadeOutInterpolation, Interpolation::Hold);
    EXPECT_DOUBLE_EQ(right.volume, original.volume);
    ASSERT_TRUE(right.volumeTrack);
    EXPECT_EQ(right.volumeTrack->value["keyframes"][0]["frame"], -7);
    EXPECT_EQ(right.volumeTrack->value["keyframes"][1]["frame"], 5);
    EXPECT_EQ(track->clipById("audio-later")->startFrame, 55);
    const auto* added = track->clipById(result.affectedIds.back());
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->mediaType, ClipType::Audio);
    EXPECT_EQ(added->sourceClipType, ClipType::Audio);
    EXPECT_EQ(added->trimStartFrame, 9);
    EXPECT_EQ(added->trimEndFrame, 286);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), before.timelines[0].tracks[0]);
    expectSingleHistoryStep(editor, before, editor.project());
}

TEST(InsertRippleTest, AutomaticTrackCreationAndDefaultSourceDurationAreOneStableEdit) {
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        SCOPED_TRACE(toString(type));
        auto before = makeProject({makeClip("video-stays", 0, 20)},
                                  {makeClip("audio-stays", 0, 20, ClipType::Audio)});
        auto& tracks = before.timelines[0].tracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const Track& track) { return track.type == type; }), tracks.end());
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
        const auto result = editor.insertClip(type == ClipType::Video ? "video-media" : "audio-media",
                                              4, -1, 290);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_EQ(result.affectedIds.size(), 1u);
        ASSERT_EQ(editor.activeTimeline()->tracks.size(), 2u);
        const auto* created = editor.activeTimeline()->trackForClip(result.affectedIds.back());
        ASSERT_NE(created, nullptr);
        EXPECT_EQ(created->type, type);
        const auto& added = created->clips.front();
        EXPECT_EQ(added.durationFrames, 10);
        EXPECT_EQ(added.trimStartFrame, 290);
        EXPECT_EQ(added.trimEndFrame, 0);
        EXPECT_EQ(added.mediaType, type);
        EXPECT_EQ(added.sourceClipType, type);
        EXPECT_EQ(editor.activeTimeline()->tracks.front().type, ClipType::Video);
        EXPECT_EQ(*editor.activeTimeline()->trackById(tracks.front().id), tracks.front());
        expectSingleHistoryStep(editor, before, editor.project());
    }
}

TEST(InsertRippleTest, InsertAndOverwriteShareValidationWithoutChangingStateOrHistory) {
    struct Request {
        std::string media;
        int64_t start, duration, trim;
        std::string track;
        EditStatus expected = EditStatus::Invalid;
    };
    const int64_t max = std::numeric_limits<int64_t>::max();
    const std::vector<Request> requests{
        {"missing", 0, 5, 0, {}},
        {"video-media", -1, 5, 0, {}},
        {"video-media", 0, 0, 0, {}},
        {"video-media", 0, 5, -1, {}},
        {"video-media", 0, 5, 301, {}},
        {"video-media", 0, -1, 300, {}},
        {"video-media", 0, 11, 290, {}},
        {"video-media", 0, 5, 0, "missing"},
        {"video-media", 0, 5, 0, "audio"},
        {"audio-media", 0, 5, 0, "video"},
        {"image-media", 0, 5, 0, {}, EditStatus::Unsupported},
        {"huge-media", 0, 5, 0, {}},
        {"video-media", max - 5, 5, 0, {}}, // End exactly INT64_MAX is invalid too.
        {"video-media", max - 3, 5, 0, {}},
    };
    EditorViewModel editor;
    const auto before = makeProject({makeClip("original", 0, 20)});
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    const auto revision = editor.revision();
    for (const auto& request : requests) {
        SCOPED_TRACE(request.media + " / " + std::to_string(request.start) + " / " + request.track);
        const auto insert = editor.insertClip(request.media, request.start, request.duration, request.trim, request.track);
        const auto overwrite = editor.placeClip(request.media, request.start, request.duration, request.trim, request.track);
        EXPECT_EQ(insert.status, request.expected);
        EXPECT_EQ(insert.status, overwrite.status);
        EXPECT_EQ(insert.message, overwrite.message);
        EXPECT_TRUE(insert.affectedIds.empty());
        EXPECT_EQ(editor.project(), before);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_FALSE(editor.timelines().canUndo());
        EXPECT_FALSE(editor.timelines().canRedo());
    }
}

TEST(InsertRippleTest, OverwriteStillClearsRegionInsteadOfRippling) {
    const auto before = makeProject({makeClip("original", 0, 20), makeClip("later", 30, 5)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    const auto result = editor.placeClip("video-media", 5, 5, 2);
    ASSERT_TRUE(result.changed()) << result.message;
    const auto* track = editor.activeTimeline()->trackById("video");
    ASSERT_EQ(track->clips.size(), 4u);
    EXPECT_EQ(track->clips[0].id, "original");
    EXPECT_EQ(track->clips[0].durationFrames, 5);
    EXPECT_EQ(track->clips[1].id, result.affectedIds.back());
    EXPECT_EQ(track->clips[1].trimStartFrame, 2);
    EXPECT_EQ(track->clips[2].startFrame, 10);
    EXPECT_EQ(track->clips[2].durationFrames, 10);
    EXPECT_EQ(track->clips[2].trimStartFrame, 10);
    EXPECT_EQ(track->clips[3], before.timelines[0].tracks[0].clips[1]);
    expectSingleHistoryStep(editor, before, editor.project());
}

TEST(InsertRippleTest, ShiftedRangeOverflowRejectsAtomicallyAndKeepsRedo) {
    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto before = makeProject({makeClip("near-limit", max - 10, 5)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    // The last valid end is MAX - 1; MAX itself is the model's overflow sentinel.
    ASSERT_TRUE(editor.insertClip("video-media", 0, 4).changed());
    EXPECT_EQ(editor.activeTimeline()->clipById("near-limit")->endFrame(), max - 1);
    const auto validAfter = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto revision = editor.revision();
    for (const auto [point, duration] : std::vector<std::pair<int64_t, int64_t>>{
             {0, 5}, {0, 6}, {0, 11}, {max - 8, 5}}) {
        SCOPED_TRACE(std::to_string(point) + " / " + std::to_string(duration));
        const auto result = editor.insertClip("video-media", point, duration);
        EXPECT_EQ(result.status, EditStatus::Invalid);
        EXPECT_TRUE(result.affectedIds.empty());
        EXPECT_EQ(editor.project(), before);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_FALSE(editor.timelines().canUndo());
        EXPECT_TRUE(editor.timelines().canRedo());
    }
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), validAfter);
}

TEST(InsertRippleTest, InsertDoesNotOverflowOrShiftTheOtherTrack) {
    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto before = makeProject({makeClip("video-original", 0, 10)},
                                    {makeClip("audio-near-limit", max - 10, 5, ClipType::Audio)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    ASSERT_TRUE(editor.insertClip("video-media", 0, 20, 0, "video").changed());
    EXPECT_EQ(editor.activeTimeline()->clipById("video-original")->startFrame, 20);
    EXPECT_EQ(*editor.activeTimeline()->trackById("audio"), before.timelines[0].tracks[1]);
    expectSingleHistoryStep(editor, before, editor.project());
}

TEST(InsertRippleTest, RippleDeleteClosesOnlyTheDeletedDurationAndPreservesEveryOtherGap) {
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        for (const int64_t nextStart : {15, 20}) {
            SCOPED_TRACE(std::string(toString(type)) + " / " + std::to_string(nextStart));
            const std::vector<Clip> target{
                makeClip("before", 2, 3, type), makeClip("delete", 10, 5, type),
                makeClip("next", nextStart, 5, type), animatedClip("later", 40, type)};
            const std::vector<Clip> other{makeClip("other", 8, 30,
                type == ClipType::Video ? ClipType::Audio : ClipType::Video)};
            const auto before = type == ClipType::Video ? makeProject(target, other) : makeProject(other, target);
            EditorViewModel editor;
            ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
            const auto revision = editor.revision();
            const auto result = editor.rippleDeleteClip("delete");
            ASSERT_TRUE(result.changed()) << result.message;
            EXPECT_EQ(result.affectedIds, (std::vector<std::string>{"delete", "next", "later"}));
            EXPECT_EQ(editor.revision(), revision + 1);
            const auto* track = editor.activeTimeline()->trackById(type == ClipType::Video ? "video" : "audio");
            ASSERT_EQ(track->clips.size(), 3u);
            EXPECT_EQ(track->clipById("delete"), nullptr);
            EXPECT_EQ(*track->clipById("before"), target[0]);
            for (const auto index : {2u, 3u}) {
                auto expected = target[index];
                expected.startFrame -= 5;
                EXPECT_EQ(*track->clipById(expected.id), expected); // Source/effects/fades remain exact.
            }
            EXPECT_EQ(track->clips[1].startFrame - track->clips[0].endFrame(),
                      (target[1].startFrame - target[0].endFrame()) + (target[2].startFrame - target[1].endFrame()));
            EXPECT_EQ(track->clips[2].startFrame - track->clips[1].endFrame(),
                      target[3].startFrame - target[2].endFrame());
            const auto* untouched = editor.activeTimeline()->trackById(type == ClipType::Video ? "audio" : "video");
            EXPECT_EQ(untouched->clips, other);
            expectSingleHistoryStep(editor, before, editor.project());
        }
    }
}

TEST(InsertRippleTest, RippleDeleteTailOrOnlyClipRetainsTrackAndOneStepHistory) {
    for (const bool onlyClip : {false, true}) {
        SCOPED_TRACE(onlyClip);
        std::vector<Clip> clips;
        if (!onlyClip) clips.push_back(makeClip("earlier", 2, 5));
        clips.push_back(makeClip("tail", 20, 10));
        const auto before = makeProject(clips, {makeClip("audio-stays", 10, 30, ClipType::Audio)});
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
        const auto result = editor.rippleDeleteClip("tail");
        ASSERT_TRUE(result.changed()) << result.message;
        EXPECT_EQ(result.affectedIds, (std::vector<std::string>{"tail"}));
        EXPECT_EQ(editor.activeTimeline()->tracks.size(), 2u);
        const auto* track = editor.activeTimeline()->trackById("video");
        ASSERT_NE(track, nullptr);
        EXPECT_EQ(track->clips.size(), onlyClip ? 0u : 1u);
        if (!onlyClip) EXPECT_EQ(track->clips.front(), clips.front());
        EXPECT_EQ(*editor.activeTimeline()->trackById("audio"), before.timelines[0].tracks[1]);
        expectSingleHistoryStep(editor, before, editor.project());
    }
}

TEST(InsertRippleTest, RippleDeleteNearFrameLimitUsesDurationWithoutRepackingGaps) {
    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto before = makeProject({makeClip("delete", max - 30, 5), makeClip("later", max - 10, 5)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    ASSERT_TRUE(editor.rippleDeleteClip("delete").changed());
    const auto* later = editor.activeTimeline()->clipById("later");
    ASSERT_NE(later, nullptr);
    EXPECT_EQ(later->startFrame, max - 15);
    EXPECT_EQ(later->endFrame(), max - 10);
    expectSingleHistoryStep(editor, before, editor.project());
}

TEST(InsertRippleTest, MissingRippleDeleteDoesNotCreateHistoryOrClearRedo) {
    const auto before = makeProject({makeClip("original", 0, 20)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    const auto revision = editor.revision();
    EXPECT_EQ(editor.rippleDeleteClip("missing").status, EditStatus::Invalid);
    EXPECT_EQ(editor.rippleDeleteClip("").status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.rippleDeleteClip("original").changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto undoneRevision = editor.revision();
    EXPECT_EQ(editor.rippleDeleteClip("missing").status, EditStatus::Invalid);
    EXPECT_EQ(editor.revision(), undoneRevision);
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.timelines().canUndo());
    EXPECT_TRUE(editor.timelines().canRedo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST(InsertRippleTest, UnsupportedTrackLayoutIsRejectedWithoutHistory) {
    auto before = makeProject({makeClip("original", 0, 20)});
    Track extra;
    extra.id = "extra-video";
    before.timelines[0].tracks.push_back(extra);
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    const auto revision = editor.revision();
    EXPECT_EQ(editor.insertClip("video-media", 0, 5).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.rippleDeleteClip("original").status, EditStatus::Unsupported);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.undo());
}

TEST(InsertRippleTest, PendingEffectsTransactionBlocksInsertAndRippleDeleteWithoutHistory) {
    const auto before = makeProject({makeClip("original", 0, 20)},
                                    {makeClip("audio-original", 0, 20, ClipType::Audio)});
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, makeManifest()));
    ASSERT_TRUE(editor.beginClipEffectsTransaction("original").changed());
    ASSERT_TRUE(editor.updateClipEffectsTransaction(animatedClip("effects", 0).effects).changed());
    const auto preview = editor.project();
    const auto revision = editor.revision();
    EXPECT_EQ(editor.insertClip("video-media", 5, 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor.insertClip("audio-media", 5, 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor.rippleDeleteClip("original").status, EditStatus::Invalid);
    EXPECT_EQ(editor.rippleDeleteClip("audio-original").status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), preview);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
    EXPECT_FALSE(editor.timelines().canRedo());
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.cancelClipEffectsTransaction().changed());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.insertClip("video-media", 5, 5).changed());
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    ASSERT_TRUE(editor.rippleDeleteClip("audio-original").changed());
    EXPECT_FALSE(editor.timelines().canRedo());
    expectSingleHistoryStep(editor, before, editor.project());
}
