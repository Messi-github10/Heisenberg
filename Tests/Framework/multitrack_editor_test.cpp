#include <Editor/EditorViewModel.hpp>
#include <Models/ProjectCodec.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace heisenberg;

Clip multitrackClip(const std::string& id, ClipType type, int64_t start = 0, int64_t duration = 10) {
    Clip clip;
    clip.id = id;
    clip.mediaRef = type == ClipType::Video ? "video-media" : "audio-media";
    clip.mediaType = clip.sourceClipType = type;
    clip.startFrame = start;
    clip.durationFrames = duration;
    clip.trimStartFrame = 3;
    clip.trimEndFrame = 7;
    clip.speed = 1.5;
    clip.volume = 0.6;
    clip.linkGroupId = "preserve-link";
    clip.captionGroupId = "preserve-caption";
    clip.wordTimings = std::vector<WordTiming>{{"word", 1, 4}};
    clip.volumeTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[{"frame":2,"value":0.8}],"opaque":"keep"})")};
    if (type == ClipType::Video) {
        clip.opacity = 0.75;
        clip.effects = JsonValue::parse(R"([{"id":"opaque-effect","type":"future.effect","params":{"data":[1,2,3]}}])");
        clip.opacityTrack = clip.volumeTrack;
    }
    return clip;
}

ProjectFile multitrackProject() {
    ProjectFile project;
    Timeline timeline;
    timeline.id = "timeline";
    for (const auto& id : {"v-top", "v-bottom", "a-top", "a-bottom"}) {
        Track track;
        track.id = id;
        track.type = id[0] == 'v' ? ClipType::Video : ClipType::Audio;
        track.clips.push_back(multitrackClip(std::string(id) + "-clip", track.type));
        timeline.tracks.push_back(std::move(track));
    }
    project.timelines.push_back(std::move(timeline));
    project.activeTimelineId = "timeline";
    return project;
}

MediaManifest multitrackManifest() {
    MediaManifest manifest;
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        MediaManifestEntry entry;
        entry.id = type == ClipType::Video ? "video-media" : "audio-media";
        entry.type = type;
        entry.duration = 10.0;
        entry.source = {MediaSource::Kind::External, "C:/offline-multitrack.media"};
        EXPECT_TRUE(manifest.add(entry));
    }
    return manifest;
}

void expectOneEdit(EditorViewModel& editor, const ProjectFile& before, ProjectFile after) {
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
    EXPECT_FALSE(editor.redo());
}

void expectOtherTracks(const EditorViewModel& editor, const ProjectFile& before,
                       const std::vector<std::string>& changed) {
    for (const auto& track : before.timelines.front().tracks) {
        if (std::find(changed.begin(), changed.end(), track.id) != changed.end()) continue;
        const auto* actual = editor.activeTimeline()->trackById(track.id);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(*actual, track);
    }
}
} // namespace

TEST(MultitrackEditorTest, RangePlacementUsesTopOrNewVideoTrackWithOneExactUndo) {
    const auto before = multitrackProject();
    for (const bool createNewTrack : {false, true}) {
        SCOPED_TRACE(createNewTrack);
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto revision = editor.revision();
        const auto result = editor.placeClip("video-media", 4, 6, 12, {}, createNewTrack);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_FALSE(result.affectedIds.empty());
        const auto* clip = editor.activeTimeline()->clipById(result.affectedIds.back());
        ASSERT_NE(clip, nullptr);
        EXPECT_EQ(clip->mediaRef, "video-media");
        EXPECT_EQ(clip->startFrame, 4);
        EXPECT_EQ(clip->durationFrames, 6);
        EXPECT_EQ(clip->trimStartFrame, 12);
        EXPECT_EQ(clip->trimEndFrame, 10 * editor.activeTimeline()->fps - 18);
        const auto* owner = editor.activeTimeline()->trackForClip(clip->id);
        ASSERT_NE(owner, nullptr);
        EXPECT_EQ(owner->id, editor.activeTimeline()->tracks.front().id);
        EXPECT_EQ(editor.activeTimeline()->tracks.size(), createNewTrack ? 5u : 4u);
        if (createNewTrack) {
            EXPECT_NE(owner->id, "v-top");
            EXPECT_EQ(owner->clips.size(), 1u);
            EXPECT_EQ(result.affectedIds, (std::vector<std::string>{owner->id, clip->id}));
        } else {
            EXPECT_EQ(owner->id, "v-top");
            EXPECT_EQ(owner->clips.size(), 2u);
        }
        expectOtherTracks(editor, before, createNewTrack ? std::vector<std::string>{}
                                                        : std::vector<std::string>{"v-top"});
        EXPECT_EQ(editor.revision(), revision + 1);
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, NewTrackInsertIsAtomicAndDoesNotRippleExistingTracks) {
    const auto before = multitrackProject();
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    const auto revision = editor.revision();
    const auto result = editor.insertClip("video-media", 4, 3, 8, {}, true);
    ASSERT_TRUE(result.changed()) << result.message;
    ASSERT_EQ(result.affectedIds.size(), 2u);
    ASSERT_EQ(editor.activeTimeline()->tracks.size(), 5u);
    const auto& track = editor.activeTimeline()->tracks.front();
    EXPECT_EQ(track.id, result.affectedIds.front());
    ASSERT_EQ(track.clips.size(), 1u);
    EXPECT_EQ(track.clips.front().id, result.affectedIds.back());
    EXPECT_EQ(track.clips.front().startFrame, 4);
    EXPECT_EQ(track.clips.front().durationFrames, 3);
    EXPECT_EQ(track.clips.front().trimStartFrame, 8);
    EXPECT_EQ(track.clips.front().trimEndFrame, 10 * editor.activeTimeline()->fps - 11);
    expectOtherTracks(editor, before, {});
    EXPECT_EQ(editor.revision(), revision + 1);
    expectOneEdit(editor, before, editor.project());
}

TEST(MultitrackEditorTest, ExistingAndNewTrackMovesPreservePayloadAndSourceAtSameOrNewFrame) {
    for (const bool createNewTrack : {false, true}) {
        for (const int64_t frame : {int64_t{0}, int64_t{20}}) {
            SCOPED_TRACE(createNewTrack);
            SCOPED_TRACE(frame);
            const auto before = multitrackProject();
            EditorViewModel editor;
            ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
            const auto revision = editor.revision();
            const auto result = editor.moveClip("v-top-clip", frame, createNewTrack ? "" : "v-bottom",
                                                createNewTrack);
            ASSERT_TRUE(result.changed()) << result.message;
            ASSERT_FALSE(result.affectedIds.empty());
            EXPECT_EQ(result.affectedIds.back(), "v-top-clip");
            auto expected = before.timelines.front().trackById("v-top")->clips.front();
            expected.startFrame = frame;
            const auto* moved = editor.activeTimeline()->clipById(expected.id);
            ASSERT_NE(moved, nullptr);
            EXPECT_EQ(*moved, expected);
            const auto* source = editor.activeTimeline()->trackById("v-top");
            ASSERT_NE(source, nullptr);
            auto expectedSource = *before.timelines.front().trackById("v-top");
            expectedSource.clips.clear();
            EXPECT_EQ(*source, expectedSource);
            const auto* owner = editor.activeTimeline()->trackForClip(expected.id);
            ASSERT_NE(owner, nullptr);
            EXPECT_EQ(editor.activeTimeline()->tracks.size(), createNewTrack ? 5u : 4u);
            if (createNewTrack) {
                EXPECT_EQ(owner->id, editor.activeTimeline()->tracks.front().id);
                EXPECT_NE(owner->id, source->id);
                EXPECT_NE(owner->id, "v-bottom");
                EXPECT_EQ(result.affectedIds, (std::vector<std::string>{owner->id, expected.id}));
                EXPECT_EQ(owner->clips.size(), 1u);
            } else {
                EXPECT_EQ(owner->id, "v-bottom");
            }
            expectOtherTracks(editor, before, createNewTrack ? std::vector<std::string>{"v-top"}
                                                            : std::vector<std::string>{"v-top", "v-bottom"});
            EXPECT_EQ(editor.revision(), revision + 1);
            expectOneEdit(editor, before, editor.project());
        }
    }
}

TEST(MultitrackEditorTest, RejectedNewTrackRequestsLeaveNoTrackAndPreserveRevisionAndRedo) {
    const auto before = multitrackProject();
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.renameTrack("v-top", "Redo survives").changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto revision = editor.revision();
    const auto maxFrame = std::numeric_limits<int64_t>::max();
    const int64_t sourceDuration = 10 * editor.activeTimeline()->fps;
    const std::vector<std::function<EditResult()>> invalid{
        [&] { return editor.placeClip("missing", 0, 3, 0, {}, true); },
        [&] { return editor.insertClip("missing", 0, 3, 0, {}, true); },
        [&] { return editor.moveClip("missing", 0, {}, true); },
        [&] { return editor.placeClip("video-media", 0, 3, 0, "v-top", true); },
        [&] { return editor.insertClip("video-media", 0, 3, 0, "missing", true); },
        [&] { return editor.moveClip("v-top-clip", 0, "v-bottom", true); },
        [&] { return editor.moveClip("v-top-clip", 0, "missing", true); },
        [&] { return editor.placeClip("video-media", -1, 3, 0, {}, true); },
        [&] { return editor.placeClip("video-media", 0, 3, -1, {}, true); },
        [&] { return editor.placeClip("video-media", 0, 0, 0, {}, true); },
        [&] { return editor.placeClip("video-media", 0, 4, sourceDuration - 1, {}, true); },
        [&] { return editor.placeClip("video-media", 0, -1, sourceDuration + 1, {}, true); },
        [&] { return editor.insertClip("video-media", 0, 4, sourceDuration - 1, {}, true); },
        [&] { return editor.insertClip("video-media", maxFrame - 3, 3, 0, {}, true); },
        [&] { return editor.placeClip("video-media", maxFrame - 2, 3, 0, {}, true); },
        [&] { return editor.moveClip("v-top-clip", -1, {}, true); },
        [&] { return editor.moveClip("v-top-clip", maxFrame - 10, {}, true); },
        [&] { return editor.moveClip("v-top-clip", maxFrame, {}, true); },
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        const auto result = invalid[index]();
        EXPECT_EQ(result.status, EditStatus::Invalid);
        EXPECT_TRUE(result.affectedIds.empty());
        EXPECT_EQ(editor.project(), before);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_FALSE(editor.timelines().canUndo());
        EXPECT_TRUE(editor.timelines().canRedo());
    }
    EXPECT_EQ(editor.placeClip("audio-media", 0, 3, 0, {}, true).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.insertClip("audio-media", 0, 3, 0, {}, true).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.moveClip("a-top-clip", 0, {}, true).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST(MultitrackEditorTest, EffectsTransactionBlocksAtomicNewTrackEditsAndPreservesRedo) {
    const auto before = multitrackProject();
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.renameTrack("v-top", "Redo survives").changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    ASSERT_TRUE(editor.beginClipEffectsTransaction("v-top-clip").changed());
    ASSERT_TRUE(editor.updateClipEffectsTransaction(JsonValue::array()).changed());
    const auto preview = editor.project();
    const auto revision = editor.revision();
    EXPECT_EQ(editor.placeClip("video-media", 4, 3, 8, {}, true).status, EditStatus::Invalid);
    EXPECT_EQ(editor.insertClip("video-media", 4, 3, 8, {}, true).status, EditStatus::Invalid);
    EXPECT_EQ(editor.moveClip("v-top-clip", 0, {}, true).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), preview);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
    EXPECT_TRUE(editor.timelines().canRedo());
    ASSERT_TRUE(editor.cancelClipEffectsTransaction().changed());
    EXPECT_EQ(editor.project(), before);
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST(MultitrackEditorTest, NewVideoTrackPrecedesFirstVideoAndPreservesInterleavedOrder) {
    auto before = multitrackProject();
    auto& tracks = before.timelines.front().tracks;
    std::swap(tracks[0], tracks[2]);
    std::swap(tracks[2], tracks[3]);
    // Audio, video, audio, video: the leading audio stays ahead of the new video.
    const std::vector<std::function<EditResult(EditorViewModel&)>> operations{
        [](auto& editor) { return editor.addTrack(ClipType::Video); },
        [](auto& editor) { return editor.placeClip("video-media", 4, 3, 0, {}, true); },
        [](auto& editor) { return editor.insertClip("video-media", 4, 3, 0, {}, true); },
        [](auto& editor) { return editor.moveClip("v-top-clip", 0, {}, true); },
    };
    for (size_t index = 0; index < operations.size(); ++index) {
        SCOPED_TRACE(index);
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto revision = editor.revision();
        const auto result = operations[index](editor);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_FALSE(result.affectedIds.empty());
        const auto& actual = editor.activeTimeline()->tracks;
        ASSERT_EQ(actual.size(), 5u);
        EXPECT_EQ(actual[0], tracks[0]);
        EXPECT_EQ(actual[1].id, result.affectedIds.front());
        EXPECT_EQ(actual[1].type, ClipType::Video);
        EXPECT_EQ(actual[2].id, tracks[1].id);
        EXPECT_EQ(actual[3], tracks[2]);
        EXPECT_EQ(actual[4].id, tracks[3].id);
        expectOtherTracks(editor, before, index == 3 ? std::vector<std::string>{"v-top"}
                                                   : std::vector<std::string>{});
        EXPECT_EQ(editor.revision(), revision + 1);
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, AddTrackUsesTopFirstVideoAndAppendAudioWithStableIds) {
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        const auto before = multitrackProject();
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto revision = editor.revision();
        const auto result = editor.addTrack(type);
        ASSERT_TRUE(result.changed()) << result.message;
        ASSERT_EQ(result.affectedIds.size(), 1u);
        const auto id = result.affectedIds.front();
        EXPECT_EQ(id.size(), 36u);
        EXPECT_EQ(editor.revision(), revision + 1);
        ASSERT_EQ(editor.activeTimeline()->tracks.size(), 5u);
        EXPECT_EQ(editor.activeTimeline()->tracks[type == ClipType::Video ? 0 : 4].id, id);
        EXPECT_EQ(editor.activeTimeline()->trackById(id)->type, type);
        EXPECT_TRUE(editor.activeTimeline()->trackById(id)->clips.empty());
        expectOtherTracks(editor, before, {});
        if (type == ClipType::Video) EXPECT_EQ(editor.activeTrack()->id, id);
        expectOneEdit(editor, before, editor.project());
    }
    EditorViewModel editor;
    ASSERT_TRUE(editor.addTrack(ClipType::Audio).changed());
    ASSERT_TRUE(editor.addTrack(ClipType::Video).changed());
    EXPECT_EQ(editor.activeTimeline()->tracks.front().type, ClipType::Video);
    EXPECT_EQ(editor.activeTimeline()->tracks.back().type, ClipType::Audio);
}

TEST(MultitrackEditorTest, TrackMetadataReorderAndRemovalEachCommitOneExactSnapshot) {
    const auto before = multitrackProject();
    const std::vector<std::function<EditResult(EditorViewModel&)>> operations{
        [](auto& editor) { return editor.renameTrack("v-top", "Picture layer"); },
        [](auto& editor) { return editor.setTrackHidden("v-top", true); },
        [](auto& editor) { return editor.setTrackMuted("a-top", true); },
        [](auto& editor) { return editor.reorderTrack("v-top", 1); },
        [](auto& editor) { return editor.reorderTrack("v-bottom", 0); },
        [](auto& editor) { return editor.reorderTrack("a-top", 3); },
        [](auto& editor) { return editor.reorderTrack("a-bottom", 2); },
        [](auto& editor) { return editor.removeTrack("v-top"); },
        [](auto& editor) { return editor.removeTrack("a-bottom"); },
    };
    for (size_t index = 0; index < operations.size(); ++index) {
        SCOPED_TRACE(index);
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto revision = editor.revision();
        const auto result = operations[index](editor);
        ASSERT_TRUE(result.changed()) << result.message;
        EXPECT_EQ(editor.revision(), revision + 1);
        EXPECT_FALSE(result.affectedIds.empty());
        if (index == 0) EXPECT_EQ(editor.activeTimeline()->trackById("v-top")->name, "Picture layer");
        if (index == 1) EXPECT_TRUE(editor.activeTimeline()->trackById("v-top")->hidden);
        if (index == 2) EXPECT_TRUE(editor.activeTimeline()->trackById("a-top")->muted);
        if (index == 3 || index == 4) EXPECT_EQ(editor.activeTimeline()->tracks.front().id, "v-bottom");
        if (index == 5 || index == 6) EXPECT_EQ(editor.activeTimeline()->tracks[2].id, "a-bottom");
        if (index >= 7) {
            EXPECT_EQ(editor.activeTimeline()->trackById(result.affectedIds.front()), nullptr);
            ASSERT_EQ(result.affectedIds.size(), 2u);
            EXPECT_EQ(editor.activeTimeline()->clipById(result.affectedIds.back()), nullptr);
        }
        expectOtherTracks(editor, before, {result.affectedIds.front()});
        ProjectFile decoded;
        std::string error;
        ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(editor.project()), decoded, &error)) << error;
        EXPECT_EQ(decoded, editor.project());
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, RenameEmptyRestoresDefaultAndLastTrackCanBeRemoved) {
    auto before = multitrackProject();
    before.timelines.front().tracks.front().name = "Custom";
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.renameTrack("v-top", "").changed());
    EXPECT_FALSE(editor.activeTimeline()->trackById("v-top")->name);
    expectOneEdit(editor, before, editor.project());
    for (const auto& track : before.timelines.front().tracks) ASSERT_TRUE(editor.removeTrack(track.id).changed());
    EXPECT_TRUE(editor.activeTimeline()->tracks.empty());
    EXPECT_EQ(editor.activeTrack(), nullptr);
    EXPECT_EQ(editor.compatibility().level, CompatibilityLevel::Supported);
}

TEST(MultitrackEditorTest, ConstantOpacityPreservesAutomationPayloadAndSnapshot) {
    for (const double opacity : {0.0, 0.4, 1.0}) {
        auto before = multitrackProject();
        VideoProject project;
        auto& editor = project.editor();
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto snapshot = project.snapshot();
        const auto revision = editor.revision();
        const auto result = editor.setClipOpacity("v-top-clip", opacity);
        ASSERT_TRUE(result.changed());
        auto expected = before.timelines.front().tracks.front().clips.front();
        expected.opacity = opacity;
        EXPECT_EQ(*editor.activeTimeline()->clipById(expected.id), expected);
        EXPECT_EQ(snapshot.project(), before);
        EXPECT_EQ(editor.revision(), revision + 1);
        EXPECT_EQ(editor.compatibility().level, CompatibilityLevel::Warning);
        expectOtherTracks(editor, before, {"v-top"});
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, CrossTrackMoveOverwritesOnlyTargetAndPreservesLocalPayloadAndIds) {
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        const std::string sourceId = type == ClipType::Video ? "v-top" : "a-top";
        const std::string targetId = type == ClipType::Video ? "v-bottom" : "a-bottom";
        auto before = multitrackProject();
        auto& timeline = before.timelines.front();
        timeline.trackById(targetId)->clips = {multitrackClip("target", type, 0, 30)};
        timeline.trackById(sourceId)->clips.push_back(multitrackClip("source-neighbor", type, 40, 5));
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto revision = editor.revision();
        const auto result = editor.moveClip(sourceId + "-clip", 5, targetId);
        ASSERT_TRUE(result.changed()) << result.message;
        EXPECT_EQ(result.affectedIds.back(), sourceId + "-clip");
        EXPECT_EQ(result.affectedIds.front(), "target");
        EXPECT_EQ(editor.revision(), revision + 1);
        auto expected = timeline.trackById(sourceId)->clips.front();
        expected.startFrame = 5;
        EXPECT_EQ(*editor.activeTimeline()->clipById(expected.id), expected);
        EXPECT_EQ(editor.activeTimeline()->trackForClip(expected.id)->id, targetId);
        const auto* source = editor.activeTimeline()->trackById(sourceId);
        ASSERT_EQ(source->clips.size(), 1u);
        EXPECT_EQ(source->clips.front(), timeline.trackById(sourceId)->clips.back());
        const auto* target = editor.activeTimeline()->trackById(targetId);
        ASSERT_EQ(target->clips.size(), 3u);
        EXPECT_EQ(target->clips.front().id, "target");
        EXPECT_EQ(target->clips.front().durationFrames, 5);
        EXPECT_EQ(target->clips.back().startFrame, 15);
        EXPECT_EQ(target->clips.back().durationFrames, 15);
        EXPECT_NE(target->clips.back().id, "target");
        EXPECT_NE(target->clips.back().id, expected.id);
        EXPECT_NE(std::find(result.affectedIds.begin(), result.affectedIds.end(), target->clips.back().id),
                  result.affectedIds.end());
        expectOtherTracks(editor, before, {sourceId, targetId});
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, ExplicitTargetInsertAndRippleDeleteIsolateSameTypeTracks) {
    auto before = multitrackProject();
    before.timelines.front().trackById("v-bottom")->clips.push_back(
        multitrackClip("target-later", ClipType::Video, 20, 5));
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    const auto revision = editor.revision();
    const auto inserted = editor.insertClip("video-media", 4, 3, 0, "v-bottom");
    ASSERT_TRUE(inserted.changed()) << inserted.message;
    ASSERT_EQ(inserted.affectedIds.size(), 4u);
    const std::string insertedId = inserted.affectedIds.back();
    const std::string rightId = inserted.affectedIds[1];
    const auto* target = editor.activeTimeline()->trackById("v-bottom");
    ASSERT_EQ(target->clips.size(), 4u);
    EXPECT_EQ(target->clips[0].id, "v-bottom-clip");
    EXPECT_EQ(target->clips[0].startFrame, 0);
    EXPECT_EQ(target->clips[0].durationFrames, 4);
    EXPECT_EQ(target->clips[1].id, insertedId);
    EXPECT_EQ(target->clips[2].id, rightId);
    EXPECT_EQ(target->clips[2].startFrame, 7);
    EXPECT_EQ(target->clips[2].durationFrames, 6);
    EXPECT_EQ(target->clips[3].startFrame, 23);
    expectOtherTracks(editor, before, {"v-bottom"});
    const auto afterInsert = editor.project();
    Clip expectedRight = target->clips[2];
    expectedRight.startFrame = 4;

    const auto deleted = editor.rippleDeleteClip(insertedId);
    ASSERT_TRUE(deleted.changed()) << deleted.message;
    EXPECT_EQ(deleted.affectedIds, (std::vector<std::string>{insertedId, rightId, "target-later"}));
    target = editor.activeTimeline()->trackById("v-bottom");
    ASSERT_EQ(target->clips.size(), 3u);
    EXPECT_EQ(target->clipById(insertedId), nullptr);
    EXPECT_EQ(target->clips[0], afterInsert.timelines.front().trackById("v-bottom")->clips[0]);
    EXPECT_EQ(target->clips[1], expectedRight);
    EXPECT_EQ(target->clips[0].endFrame(), target->clips[1].startFrame);
    EXPECT_EQ(target->clips[1].endFrame(), 10);
    EXPECT_EQ(target->clips[2], before.timelines.front().trackById("v-bottom")->clips.back());
    expectOtherTracks(editor, before, {"v-bottom"});
    EXPECT_EQ(editor.revision(), revision + 2);
    const auto afterDelete = editor.project();
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), afterInsert);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), afterInsert);
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), afterDelete);
}

TEST(MultitrackEditorTest, SameFrameCrossTrackMoveChangesOwnershipAndDefaultMoveStaysOnOwner) {
    const auto before = multitrackProject();
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.moveClip("v-top-clip", 0, "v-bottom").changed());
    EXPECT_EQ(editor.activeTimeline()->trackForClip("v-top-clip")->id, "v-bottom");
    EXPECT_EQ(editor.activeTimeline()->clipById("v-bottom-clip"), nullptr);
    expectOneEdit(editor, before, editor.project());
    ASSERT_TRUE(editor.moveClip("v-top-clip", 20).changed());
    EXPECT_EQ(editor.activeTimeline()->trackForClip("v-top-clip")->id, "v-bottom");
}

TEST(MultitrackEditorTest, InvalidRequestsAndNoOpsPreserveRevisionAndRedo) {
    const auto before = multitrackProject();
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.addTrack(ClipType::Video).changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto revision = editor.revision();
    const std::vector<std::function<EditResult()>> invalid{
        [&] { return editor.removeTrack("missing"); },
        [&] { return editor.renameTrack("missing", "Name"); },
        [&] { return editor.reorderTrack("missing", 0); },
        [&] { return editor.reorderTrack("v-top", -1); },
        [&] { return editor.reorderTrack("v-top", 4); },
        [&] { return editor.reorderTrack("v-top", 2); },
        [&] { return editor.reorderTrack("a-top", 1); },
        [&] { return editor.setTrackHidden("missing", true); },
        [&] { return editor.setClipOpacity("missing", 0.5); },
        [&] { return editor.setClipOpacity("v-top-clip", -0.1); },
        [&] { return editor.setClipOpacity("v-top-clip", 1.1); },
        [&] { return editor.setClipOpacity("v-top-clip", std::numeric_limits<double>::infinity()); },
        [&] { return editor.setClipOpacity("v-top-clip", std::numeric_limits<double>::quiet_NaN()); },
        [&] { return editor.moveClip("missing", 0, "v-bottom"); },
        [&] { return editor.moveClip("v-top-clip", 0, "missing"); },
        [&] { return editor.moveClip("v-top-clip", 0, "a-top"); },
        [&] { return editor.moveClip("a-top-clip", 0, "v-top"); },
        [&] { return editor.moveClip("v-top-clip", -1, "v-bottom"); },
        [&] { return editor.moveClip("v-top-clip", std::numeric_limits<int64_t>::max() - 10, "v-bottom"); },
        [&] { return editor.moveClip("v-top-clip", std::numeric_limits<int64_t>::max(), "v-bottom"); },
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(invalid[index]().status, EditStatus::Invalid);
    }
    EXPECT_EQ(editor.addTrack(ClipType::Text).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.setClipOpacity("a-top-clip", 0.5).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.renameTrack("v-top", "").status, EditStatus::NoOp);
    EXPECT_EQ(editor.reorderTrack("v-top", 0).status, EditStatus::NoOp);
    EXPECT_EQ(editor.reorderTrack("a-bottom", 3).status, EditStatus::NoOp);
    EXPECT_EQ(editor.setTrackHidden("v-top", false).status, EditStatus::NoOp);
    EXPECT_EQ(editor.setClipOpacity("v-top-clip", 0.75).status, EditStatus::NoOp);
    EXPECT_EQ(editor.moveClip("v-top-clip", 0).status, EditStatus::NoOp);
    EXPECT_EQ(editor.moveClip("v-top-clip", 0, "v-top").status, EditStatus::NoOp);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
    EXPECT_TRUE(editor.timelines().canRedo());
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST(MultitrackEditorTest, ReorderPermutesSameTypeSlotsAndPreservesInterleavedOtherTypePositions) {
    auto before = multitrackProject();
    std::swap(before.timelines.front().tracks[1], before.timelines.front().tracks[2]);
    for (const auto type : {ClipType::Video, ClipType::Audio}) {
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
        const auto result = type == ClipType::Video ? editor.reorderTrack("v-top", 2)
                                                    : editor.reorderTrack("a-bottom", 1);
        ASSERT_TRUE(result.changed());
        const auto& tracks = editor.activeTimeline()->tracks;
        if (type == ClipType::Video) {
            EXPECT_EQ(tracks[0].id, "v-bottom");
            EXPECT_EQ(tracks[2].id, "v-top");
            EXPECT_EQ(tracks[1], before.timelines.front().tracks[1]);
            EXPECT_EQ(tracks[3], before.timelines.front().tracks[3]);
        } else {
            EXPECT_EQ(tracks[1].id, "a-bottom");
            EXPECT_EQ(tracks[3].id, "a-top");
            EXPECT_EQ(tracks[0], before.timelines.front().tracks[0]);
            EXPECT_EQ(tracks[2], before.timelines.front().tracks[2]);
        }
        expectOneEdit(editor, before, editor.project());
    }
}

TEST(MultitrackEditorTest, EffectsTransactionBlocksAllNewMutations) {
    EditorViewModel editor;
    const auto before = multitrackProject();
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    ASSERT_TRUE(editor.beginClipEffectsTransaction("v-top-clip").changed());
    ASSERT_TRUE(editor.updateClipEffectsTransaction(JsonValue::array()).changed());
    const auto preview = editor.project();
    const auto revision = editor.revision();
    EXPECT_EQ(editor.addTrack(ClipType::Video).status, EditStatus::Invalid);
    EXPECT_EQ(editor.addTrack(ClipType::Audio).status, EditStatus::Invalid);
    EXPECT_EQ(editor.removeTrack("v-top").status, EditStatus::Invalid);
    EXPECT_EQ(editor.renameTrack("v-top", "New").status, EditStatus::Invalid);
    EXPECT_EQ(editor.reorderTrack("v-top", 1).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setTrackHidden("v-top", true).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipOpacity("v-top-clip", 0.5).status, EditStatus::Invalid);
    EXPECT_EQ(editor.moveClip("v-top-clip", 0, "v-bottom").status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), preview);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.timelines().canUndo());
    EXPECT_FALSE(editor.timelines().canRedo());
    ASSERT_TRUE(editor.cancelClipEffectsTransaction().changed());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
}

TEST(MultitrackEditorTest, UnsupportedTrackTypesGateAllNewMutations) {
    auto before = multitrackProject();
    before.timelines.front().tracks.back().clips.clear();
    before.timelines.front().tracks.back().type = ClipType::Text;
    EditorViewModel editor;
    ASSERT_TRUE(editor.replaceDocument(before, multitrackManifest()));
    const auto revision = editor.revision();
    EXPECT_EQ(editor.addTrack(ClipType::Video).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.removeTrack("v-top").status, EditStatus::Unsupported);
    EXPECT_EQ(editor.renameTrack("v-top", "New").status, EditStatus::Unsupported);
    EXPECT_EQ(editor.reorderTrack("v-top", 1).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.setTrackHidden("v-top", true).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.setClipOpacity("v-top-clip", 0.5).status, EditStatus::Unsupported);
    EXPECT_EQ(editor.moveClip("v-top-clip", 0, "v-bottom").status, EditStatus::Unsupported);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.undo());
}
