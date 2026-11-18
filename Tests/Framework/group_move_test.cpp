#include <Editor/EditorViewModel.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace heisenberg;

Clip groupClip(const std::string& id, ClipType type, int64_t start, int64_t duration = 10) {
    Clip clip;
    clip.id = id;
    clip.mediaRef = type == ClipType::Video ? "video-media" : "audio-media";
    clip.mediaType = clip.sourceClipType = type;
    clip.startFrame = start;
    clip.durationFrames = duration;
    clip.trimStartFrame = 7;
    clip.trimEndFrame = 13;
    clip.speed = 1.5;
    clip.volume = 0.6;
    clip.fadeInFrames = 2;
    clip.fadeOutFrames = 3;
    clip.fadeInInterpolation = Interpolation::Hold;
    clip.fadeOutInterpolation = Interpolation::Smooth;
    clip.opacity = 0.75;
    clip.transform = {0.2, 0.3, 0.8, 0.9, 10, 20, 30, true, true};
    clip.crop = {0.1, 0.2, 0.3, 0.4};
    clip.edgeRounding = 0.1;
    clip.edgeSoftness = 0.2;
    clip.linkGroupId = "link";
    clip.captionGroupId = "caption";
    clip.multicamGroupId = "multicam";
    clip.textContent = "preserved text";
    clip.textStyle = JsonValue{{"future", 7}};
    clip.textAnimation = JsonValue{{"type", "future.animation"}};
    clip.wordTimings = std::vector<WordTiming>{{"word", 1, 4}};
    clip.textFillMode = JsonValue{{"mode", "future.fill"}};
    const KeyframeTrackPayload track{JsonValue::parse(
        R"({"keyframes":[{"frame":-4,"value":0.2},{"frame":80,"value":0.8}],"opaque":[3,1,2]})")};
    clip.opacityTrack = clip.positionTrack = clip.scaleTrack = track;
    clip.rotationTrack = clip.cropTrack = clip.volumeTrack = track;
    clip.effects = JsonValue::parse(
        R"([{"id":"opaque","type":"future.effect","params":{"keyframes":[{"frame":-5,"value":2},{"frame":90,"value":4}]},"vendor":[3,1,2]}])");
    clip.blendMode = "future.blend";
    return clip;
}

class GroupMoveTest : public testing::Test {
protected:
    EditorViewModel editor;
    ProjectFile before;

    void SetUp() override {
        Timeline timeline;
        timeline.id = "timeline";
        Track video;
        video.id = "v1";
        video.name = "Source";
        video.clips = {groupClip("a", ClipType::Video, 0), groupClip("b", ClipType::Video, 20)};
        Track audio;
        audio.id = "a1";
        audio.type = ClipType::Audio;
        audio.clips = {groupClip("sound", ClipType::Audio, 0)};
        Track destination;
        destination.id = "v2";
        destination.hidden = true;
        destination.muted = true;
        destination.displayHeight = 72;
        destination.clips = {groupClip("c", ClipType::Video, 100)};
        Track secondAudio;
        secondAudio.id = "a2";
        secondAudio.type = ClipType::Audio;
        secondAudio.clips = {groupClip("sound2", ClipType::Audio, 20)};
        timeline.tracks = {video, audio, destination, secondAudio};
        before.timelines.push_back(timeline);
        before.activeTimelineId = timeline.id;
        load();
    }

    void load() {
        MediaManifest manifest;
        for (auto type : {ClipType::Video, ClipType::Audio}) {
            MediaManifestEntry entry;
            entry.id = type == ClipType::Video ? "video-media" : "audio-media";
            entry.type = type;
            entry.duration = 30;
            entry.source = {MediaSource::Kind::External, "C:/offline-group-move.media"};
            ASSERT_TRUE(manifest.add(entry));
        }
        std::string error;
        ASSERT_TRUE(editor.replaceDocument(before, manifest, {}, &error)) << error;
    }

    void expectMoved(const std::string& id, const std::string& trackId, int64_t start) {
        auto expected = *before.timelines.front().clipById(id);
        expected.startFrame = start;
        const auto* actual = editor.activeTimeline()->clipById(id);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(*actual, expected);
        ASSERT_NE(editor.activeTimeline()->trackForClip(id), nullptr);
        EXPECT_EQ(editor.activeTimeline()->trackForClip(id)->id, trackId);
    }

    void expectOneEdit(uint64_t revision) {
        EXPECT_EQ(editor.revision(), revision + 1);
        const auto after = editor.project();
        ASSERT_TRUE(editor.undo());
        EXPECT_EQ(editor.project(), before);
        EXPECT_FALSE(editor.undo());
        ASSERT_TRUE(editor.redo());
        EXPECT_EQ(editor.project(), after);
        EXPECT_FALSE(editor.redo());
    }
};
} // namespace

TEST_F(GroupMoveTest, SameTrackMoveRemovesWholeSelectionBeforeOverwriteAndPreservesEveryPayloadField) {
    const auto revision = editor.revision();
    // a's destination overlaps b's original range; b must still be moved intact.
    const auto result = editor.moveClips({{"a", "", 15}, {"b", "", 35}});
    ASSERT_TRUE(result.changed()) << result.message;
    EXPECT_EQ(result.affectedIds, (std::vector<std::string>{"a", "b"}));
    expectMoved("a", "v1", 15);
    expectMoved("b", "v1", 35);
    ASSERT_EQ(editor.activeTimeline()->trackById("v1")->clips.size(), 2u);
    for (const auto& id : {"a1", "v2", "a2"})
        EXPECT_EQ(*editor.activeTimeline()->trackById(id), *before.timelines.front().trackById(id));
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, CrossTrackSwapUsesOriginalClipsAndOneHistoryEntry) {
    const auto revision = editor.revision();
    const auto result = editor.moveClips({{"c", "v1", 0}, {"a", "v2", 100}});
    ASSERT_TRUE(result.changed()) << result.message;
    expectMoved("c", "v1", 0);
    expectMoved("a", "v2", 100);
    expectMoved("b", "v1", 20);
    EXPECT_EQ(editor.activeTimeline()->trackById("v1")->clips.size(), 2u);
    EXPECT_EQ(editor.activeTimeline()->trackById("v2")->clips.size(), 1u);
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, EmptyTargetsResolveIndependentlyAcrossVideoAndAudioSourceTracks) {
    const auto revision = editor.revision();
    const auto result = editor.moveClips({{"a", "", 10}, {"c", "", 10}, {"sound", "", 10}, {"sound2", "", 10}});
    ASSERT_TRUE(result.changed()) << result.message;
    expectMoved("a", "v1", 10);
    expectMoved("c", "v2", 10);
    expectMoved("sound", "a1", 10);
    expectMoved("sound2", "a2", 10);
    expectMoved("b", "v1", 20);
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, ExplicitAudioTargetsSupportAtomicSwaps) {
    const auto revision = editor.revision();
    ASSERT_TRUE(editor.moveClips({{"sound", "a2", 20}, {"sound2", "a1", 0}}).changed());
    expectMoved("sound", "a2", 20);
    expectMoved("sound2", "a1", 0);
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, DestinationUnionPreservesGapAndTrimsOnlyUnselectedOverlaps) {
    auto& destination = *before.timelines.front().trackById("v2");
    destination.clips = {groupClip("left", ClipType::Video, 0),
                         groupClip("gap", ClipType::Video, 20),
                         groupClip("right", ClipType::Video, 40)};
    load();
    const auto revision = editor.revision();
    // Reverse request order exercises interval sorting without widening to [5,45).
    const auto result = editor.moveClips({{"b", "v2", 35}, {"a", "v2", 5}});
    ASSERT_TRUE(result.changed()) << result.message;
    expectMoved("a", "v2", 5);
    expectMoved("b", "v2", 35);
    expectMoved("gap", "v2", 20);
    const auto* left = editor.activeTimeline()->clipById("left");
    const auto* right = editor.activeTimeline()->clipById("right");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->startFrame, 0);
    EXPECT_EQ(left->durationFrames, 5);
    EXPECT_EQ(right->startFrame, 45);
    EXPECT_EQ(right->durationFrames, 5);
    EXPECT_EQ(editor.activeTimeline()->trackById("v2")->clips.size(), 5u);
    EXPECT_TRUE(editor.activeTimeline()->trackById("v1")->clips.empty());
    EXPECT_EQ(std::count(result.affectedIds.begin(), result.affectedIds.end(), "gap"), 0);
    EXPECT_EQ(std::count(result.affectedIds.begin(), result.affectedIds.end(), "left"), 1);
    EXPECT_EQ(std::count(result.affectedIds.begin(), result.affectedIds.end(), "right"), 1);
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, DisjointIntervalsCutOneLongDestinationClipWithoutErasingMiddle) {
    before.timelines.front().trackById("v2")->clips = {groupClip("bed", ClipType::Video, 0, 80)};
    load();
    const auto revision = editor.revision();
    const auto result = editor.moveClips({{"a", "v2", 10}, {"b", "v2", 40}});
    ASSERT_TRUE(result.changed()) << result.message;
    const auto& clips = editor.activeTimeline()->trackById("v2")->clips;
    ASSERT_EQ(clips.size(), 5u);
    EXPECT_EQ(clips[0].startFrame, 0);
    EXPECT_EQ(clips[0].durationFrames, 10);
    EXPECT_EQ(clips[1].id, "a");
    EXPECT_EQ(clips[2].startFrame, 20);
    EXPECT_EQ(clips[2].durationFrames, 20);
    EXPECT_EQ(clips[3].id, "b");
    EXPECT_EQ(clips[4].startFrame, 50);
    EXPECT_EQ(clips[4].durationFrames, 30);
    expectMoved("a", "v2", 10);
    expectMoved("b", "v2", 40);
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, AdjacentDestinationIntervalsAreAllowedAndClearedAsOneUnion) {
    before.timelines.front().trackById("v2")->clips = {groupClip("bed", ClipType::Video, 0, 40)};
    load();
    const auto revision = editor.revision();
    const auto result = editor.moveClips({{"b", "v2", 20}, {"a", "v2", 10}});
    ASSERT_TRUE(result.changed()) << result.message;
    const auto& clips = editor.activeTimeline()->trackById("v2")->clips;
    ASSERT_EQ(clips.size(), 4u);
    EXPECT_EQ(clips[0].durationFrames, 10);
    EXPECT_EQ(clips[1].id, "a");
    EXPECT_EQ(clips[2].id, "b");
    EXPECT_EQ(clips[3].startFrame, 30);
    EXPECT_EQ(clips[3].durationFrames, 10);
    ASSERT_EQ(result.affectedIds.size(), 4u);
    EXPECT_EQ(result.affectedIds[0], "bed");
    EXPECT_EQ(result.affectedIds[1], clips[3].id);
    EXPECT_EQ(result.affectedIds[2], "b");
    EXPECT_EQ(result.affectedIds[3], "a");
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, NewTrackCreatesExactlyOneTopVideoTrackAndMovesAllClipsAtomically) {
    // Preserve a leading audio track while inserting ahead of the first video.
    std::swap(before.timelines.front().tracks[0], before.timelines.front().tracks[1]);
    load();
    const auto revision = editor.revision();
    const auto result = editor.moveClips({{"b", "", 25}, {"a", "", 5}}, true);
    ASSERT_TRUE(result.changed()) << result.message;
    ASSERT_EQ(result.affectedIds.size(), 3u);
    const auto& tracks = editor.activeTimeline()->tracks;
    ASSERT_EQ(tracks.size(), before.timelines.front().tracks.size() + 1);
    EXPECT_EQ(tracks[0], before.timelines.front().tracks[0]);
    EXPECT_EQ(tracks[1].id, result.affectedIds.front());
    EXPECT_EQ(tracks[1].type, ClipType::Video);
    EXPECT_EQ(tracks[1].clips.size(), 2u);
    EXPECT_EQ(tracks[2].id, "v1");
    EXPECT_TRUE(tracks[2].clips.empty());
    EXPECT_EQ(tracks[3], before.timelines.front().tracks[2]);
    EXPECT_EQ(tracks[4], before.timelines.front().tracks[3]);
    expectMoved("a", tracks[1].id, 5);
    expectMoved("b", tracks[1].id, 25);
    EXPECT_EQ(result.affectedIds[1], "b");
    EXPECT_EQ(result.affectedIds[2], "a");
    expectOneEdit(revision);
}

TEST_F(GroupMoveTest, InvalidAndNoOpBatchesPreserveDocumentRevisionAndRedo) {
    ASSERT_TRUE(editor.renameTrack("v1", "redo survives").changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto revision = editor.revision();
    struct RequestCase {
        std::vector<ClipMoveRequest> moves;
        bool createNewTrack = false;
        EditStatus status = EditStatus::Invalid;
    };
    const int64_t max = std::numeric_limits<int64_t>::max();
    const std::vector<RequestCase> cases{
        {{{"a", "", 5}, {"missing", "", 30}}},
        {{{"a", "", 5}, {"b", "missing", 30}}},
        {{{"a", "", 5}, {"b", "a1", 30}}},
        {{{"sound", "v1", 5}}},
        {{{"a", "", 5}, {"b", "", -1}}},
        {{{"a", "", 5}, {"b", "", max}}},
        {{{"a", "", 5}, {"b", "", max - 10}}},
        {{{"a", "", 5}, {"a", "v2", 30}}},
        {{{"a", "v2", 5}, {"b", "v2", 14}}},
        {{{"a", "v2", 5}, {"b", "v2", 5}}},
        {{{"a", "", 5}, {"b", "v2", 30}}, true},
        {{{"a", "", 5}, {"c", "", 30}}, true},
        {{{"a", "", 5}, {"b", "", 14}}, true},
        {{{"a", "", 5}, {"b", "", max}}, true},
        {{{"a", "", 5}, {"missing", "", 30}}, true},
        {{{"sound", "", 5}}, true, EditStatus::Unsupported},
        {{{"a", "", 5}, {"sound", "", 30}}, true, EditStatus::Unsupported},
        {{}, false, EditStatus::NoOp},
        {{}, true, EditStatus::NoOp},
        {{{"a", "", 0}, {"b", "v1", 20}, {"sound", "", 0}}, false, EditStatus::NoOp},
    };
    for (size_t index = 0; index < cases.size(); ++index) {
        SCOPED_TRACE(index);
        const auto& request = cases[index];
        const auto result = editor.moveClips(request.moves, request.createNewTrack);
        EXPECT_EQ(result.status, request.status) << result.message;
        if (result.status != EditStatus::NoOp) EXPECT_TRUE(result.affectedIds.empty());
        EXPECT_EQ(editor.project(), before);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_FALSE(editor.timelines().canUndo());
        EXPECT_TRUE(editor.timelines().canRedo());
    }
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST_F(GroupMoveTest, UnchangedSelectedClipStillParticipatesInCollisionValidation) {
    const auto revision = editor.revision();
    EXPECT_EQ(editor.moveClips({{"a", "", 0}, {"b", "", 5}}).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
}

TEST_F(GroupMoveTest, EffectsTransactionBlocksRegularNewTrackAndEmptyBatchesWithoutChangingPreviewOrRedo) {
    ASSERT_TRUE(editor.renameTrack("v1", "redo survives").changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    ASSERT_TRUE(editor.beginClipEffectsTransaction("a").changed());
    ASSERT_TRUE(editor.updateClipEffectsTransaction(JsonValue::array()).changed());
    const auto preview = editor.project();
    const auto revision = editor.revision();
    for (bool newTrack : {false, true}) {
        EXPECT_EQ(editor.moveClips({{"a", "", 5}, {"b", "", 25}}, newTrack).status, EditStatus::Invalid);
        EXPECT_EQ(editor.moveClips({}, newTrack).status, EditStatus::Invalid);
        EXPECT_EQ(editor.project(), preview);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_FALSE(editor.timelines().canUndo());
        EXPECT_TRUE(editor.timelines().canRedo());
    }
    ASSERT_TRUE(editor.cancelClipEffectsTransaction().changed());
    EXPECT_EQ(editor.project(), before);
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST_F(GroupMoveTest, UnsupportedLayoutRejectsBatchWithoutMutation) {
    before.timelines.front().trackById("v2")->type = ClipType::Text;
    load();
    const auto revision = editor.revision();
    EXPECT_EQ(editor.moveClips({{"a", "", 5}, {"b", "", 25}}).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
}
