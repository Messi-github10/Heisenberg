#include <Editor/EditorViewModel.hpp>
#include <Models/ProjectCodec.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {
using namespace heisenberg;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.c_str()), value.size()};
}

JsonValue originalEffects() {
    // Preserve opaque fields, ordering, and animation anchors outside the clip.
    return JsonValue::parse(R"([
        {"id":"grade","type":"video.basicGrade","enabled":true,
         "vendor":{"ordered":[3,1,2],"future":null},
         "params":{"exposure":{"value":0,"track":{"keyframes":[
             {"frame":-4,"value":-1,"interpolationOut":"hold","custom":7},
             {"frame":80,"value":2,"interpolationOut":"linear"}]}}}},
        {"id":"unknown","type":"future.effect","enabled":false,"opaque":{"x":true}}
    ])");
}

JsonValue previewEffects(double exposure) {
    auto effects = originalEffects();
    effects[0]["params"]["exposure"]["value"] = exposure;
    return effects;
}

class GradeTransactionTest : public testing::Test {
protected:
    VideoProject document;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("heisenberg-grade-transaction-" + makeUuid() + ".berg");
    std::filesystem::path destination = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("heisenberg-grade-transaction-copy-" + makeUuid() + ".berg");
    std::string error;

    EditorViewModel& editor() { return document.editor(); }
    const Clip& clip() const { return *document.editor().activeTimeline()->clipById("clip"); }
    std::string encoded() const { return ProjectCodec::encode(document.editor().project()); }

    void SetUp() override {
        ProjectFile project;
        Timeline timeline;
        timeline.id = "timeline";
        Track video;
        video.id = "video";
        Clip initial;
        initial.id = "clip";
        initial.mediaRef = "video-media";
        initial.durationFrames = 30;
        initial.trimEndFrame = 270;
        initial.effects = originalEffects();
        video.clips.push_back(initial);
        timeline.tracks.push_back(video);
        Track audio;
        audio.id = "audio";
        audio.type = ClipType::Audio;
        Clip sound;
        sound.id = "sound";
        sound.mediaRef = "audio-media";
        sound.mediaType = ClipType::Audio;
        sound.sourceClipType = ClipType::Audio;
        sound.durationFrames = 30;
        audio.clips.push_back(sound);
        timeline.tracks.push_back(audio);
        project.timelines.push_back(timeline);
        project.activeTimelineId = timeline.id;
        MediaManifest manifest;
        MediaManifestEntry entry;
        entry.id = "video-media";
        entry.name = "Video";
        entry.duration = 10;
        entry.source = {MediaSource::Kind::External, "C:/offline-transaction.mp4"};
        ASSERT_TRUE(manifest.add(entry, &error)) << error;
        entry.id = "audio-media";
        entry.type = ClipType::Audio;
        entry.source.path = "C:/offline-transaction.wav";
        ASSERT_TRUE(manifest.add(entry, &error)) << error;
        ASSERT_TRUE(editor().replaceDocument(std::move(project), std::move(manifest), {}, &error)) << error;
        ASSERT_TRUE(document.saveAs(utf8(root), &error)) << error;
        ASSERT_FALSE(document.isDirty());
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::remove_all(destination, ignored);
    }
};
} // namespace

TEST_F(GradeTransactionTest, CleanCancelRestoresExactPayloadAndInvalidatesPreviewRevision) {
    const auto before = encoded();
    const auto saved = document.savedRevision();
    const auto revision = document.revision();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    EXPECT_EQ(document.revision(), revision);
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(1)).changed());
    EXPECT_EQ(document.revision(), revision + 1);
    EXPECT_EQ(clip().effects->dump(), previewEffects(1).dump());
    EXPECT_FALSE(document.isDirty());
    EXPECT_FALSE(editor().timelines().canUndo());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(2)).changed());
    EXPECT_EQ(document.revision(), revision + 2);
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_EQ(document.revision(), revision + 3);
    EXPECT_EQ(encoded(), before);
    EXPECT_EQ(clip().effects->dump(), originalEffects().dump());
    EXPECT_EQ(document.savedRevision(), saved);
    EXPECT_FALSE(document.isDirty());
    EXPECT_FALSE(editor().undo());
    EXPECT_FALSE(editor().redo());
    // A checkpoint can be saved after cancellation despite the render revision gap.
    ASSERT_TRUE(document.save(&error)) << error;
    EXPECT_EQ(document.savedRevision(), document.revision());
    EXPECT_FALSE(document.isDirty());
}

TEST_F(GradeTransactionTest, CommitCreatesOneUndoAndRedoForAllPreviews) {
    const auto before = encoded();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    for (int value = 1; value <= 5; ++value)
        ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(value)).changed());
    EXPECT_FALSE(editor().timelines().canUndo());
    const auto previewRevision = document.revision();
    ASSERT_TRUE(editor().commitClipEffectsTransaction().changed());
    EXPECT_EQ(document.revision(), previewRevision + 1);
    EXPECT_TRUE(document.isDirty());
    const auto committed = encoded();
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(encoded(), before);
    EXPECT_FALSE(editor().undo());
    ASSERT_TRUE(editor().redo());
    EXPECT_EQ(encoded(), committed);
    EXPECT_FALSE(editor().redo());
    ASSERT_TRUE(document.save(&error)) << error;
    EXPECT_FALSE(document.isDirty());
    ASSERT_TRUE(editor().undo());
    EXPECT_TRUE(document.isDirty());
    ASSERT_TRUE(editor().redo());
    // Existing revision semantics intentionally count history navigation as edits.
    EXPECT_TRUE(document.isDirty());
    ASSERT_TRUE(document.save(&error)) << error;
    VideoProject loaded;
    ASSERT_TRUE(loaded.loadPackage(utf8(root), &error)) << error;
    EXPECT_EQ(loaded.editor().activeTimeline()->clipById("clip")->effects->dump(), previewEffects(5).dump());
    EXPECT_FALSE(loaded.isDirty());
}

TEST_F(GradeTransactionTest, DirtyCancelPreservesUndoAndRedoStacks) {
    ASSERT_TRUE(editor().moveClip("clip", 10).changed());
    ASSERT_TRUE(editor().setTrackMuted("audio", true).changed());
    ASSERT_TRUE(editor().undo());
    ASSERT_TRUE(document.isDirty());
    const auto before = encoded();
    const auto saved = document.savedRevision();
    ASSERT_TRUE(editor().timelines().canUndo());
    ASSERT_TRUE(editor().timelines().canRedo());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(std::nullopt).changed());
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_EQ(encoded(), before);
    EXPECT_TRUE(document.isDirty());
    EXPECT_EQ(document.savedRevision(), saved);
    ASSERT_TRUE(editor().redo());
    EXPECT_TRUE(editor().activeTimeline()->trackById("audio")->muted);
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(encoded(), before);
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(clip().startFrame, 0);
    EXPECT_FALSE(editor().undo());
}

TEST_F(GradeTransactionTest, NoOpCommitAndCancelPreserveCheckpointAndBothHistoryStacks) {
    ASSERT_TRUE(editor().moveClip("clip", 10).changed());
    ASSERT_TRUE(editor().setTrackMuted("audio", true).changed());
    ASSERT_TRUE(editor().undo());
    ASSERT_TRUE(document.save(&error)) << error;
    const auto saved = document.savedRevision();
    const auto revision = document.revision();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    EXPECT_EQ(editor().updateClipEffectsTransaction(originalEffects()).status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision);
    EXPECT_EQ(editor().commitClipEffectsTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision);
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(3)).changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(originalEffects()).changed());
    EXPECT_EQ(editor().commitClipEffectsTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision + 2);
    EXPECT_EQ(document.savedRevision(), saved);
    EXPECT_FALSE(document.isDirty());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    EXPECT_EQ(editor().cancelClipEffectsTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision + 2);
    ASSERT_TRUE(editor().redo());
    EXPECT_TRUE(editor().activeTimeline()->trackById("audio")->muted);
    ASSERT_TRUE(editor().undo());
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(clip().startFrame, 0);
    EXPECT_FALSE(editor().undo());
}

TEST_F(GradeTransactionTest, ChangedCommitClearsRedoButRetainsEarlierUndo) {
    ASSERT_TRUE(editor().moveClip("clip", 10).changed());
    ASSERT_TRUE(editor().setTrackMuted("audio", true).changed());
    ASSERT_TRUE(editor().undo());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(4)).changed());
    EXPECT_TRUE(editor().timelines().canRedo());
    ASSERT_TRUE(editor().commitClipEffectsTransaction().changed());
    EXPECT_FALSE(editor().redo());
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(clip().effects->dump(), originalEffects().dump());
    EXPECT_EQ(clip().startFrame, 10);
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(clip().startFrame, 0);
    EXPECT_FALSE(editor().undo());
}

TEST_F(GradeTransactionTest, PendingTransactionRejectsOtherEditsHistoryLoadsAndAllSaves) {
    ASSERT_TRUE(editor().moveClip("clip", 10).changed());
    ASSERT_TRUE(editor().undo());
    const auto oldCapture = document.snapshot();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(1)).changed());
    const auto before = encoded();
    const auto revision = document.revision();
    const auto saved = document.savedRevision();
    const auto settings = TimelineSettings::fromTimelineSettings(1280, 720, 25);
    EXPECT_EQ(editor().beginClipEffectsTransaction("clip").status, EditStatus::Invalid);
    EXPECT_EQ(editor().setClipEffects("clip", std::nullopt).status, EditStatus::Invalid);
    EXPECT_EQ(editor().moveClip("clip", 20).status, EditStatus::Invalid);
    EXPECT_EQ(editor().splitClip("clip", 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor().trimClip("clip", TrimEdge::Right, -1).status, EditStatus::Invalid);
    EXPECT_EQ(editor().removeClips({"clip"}).status, EditStatus::Invalid);
    EXPECT_EQ(editor().placeClip("video-media", 40, 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setClipAudio("clip", 0.5, 0, 0).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setTrackMuted("audio", true).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setColorSettings(editor().project().colorSettings).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setMediaInputColorSpace("video-media", "").status, EditStatus::Invalid);
    EXPECT_EQ(editor().applyTimelineSettings(settings).status, EditStatus::Invalid);
    editor().setProfile(settings);
    EXPECT_FALSE(editor().importMedia("missing.mp4", &error));
    EXPECT_FALSE(editor().undo());
    EXPECT_FALSE(editor().redo());
    EXPECT_FALSE(editor().replaceDocument(oldCapture.project(), oldCapture.manifest(), {}, &error));
    EXPECT_FALSE(document.loadPackage(utf8(root), &error));
    EXPECT_FALSE(document.save(&error));
    EXPECT_FALSE(document.saveAs(utf8(destination), &error));
    EXPECT_FALSE(document.savePackage(utf8(destination), &error));
    EXPECT_FALSE(document.saveSnapshot(oldCapture, utf8(destination), &error));
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(encoded(), before);
    EXPECT_EQ(document.revision(), revision);
    EXPECT_EQ(document.savedRevision(), saved);
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_TRUE(editor().redo());
}

TEST_F(GradeTransactionTest, SnapshotsOwnPreviewAndOriginalAndTransientSnapshotsCannotBeSavedLater) {
    const auto original = document.snapshot();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    const auto pendingWithoutChanges = document.snapshot();
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(1)).changed());
    const auto first = document.snapshot();
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(2)).changed());
    const auto second = document.snapshot();
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_EQ(original.activeTimeline()->clipById("clip")->effects->dump(), originalEffects().dump());
    EXPECT_EQ(first.activeTimeline()->clipById("clip")->effects->dump(), previewEffects(1).dump());
    EXPECT_EQ(second.activeTimeline()->clipById("clip")->effects->dump(), previewEffects(2).dump());
    EXPECT_LT(original.revision(), first.revision());
    EXPECT_LT(first.revision(), second.revision());
    EXPECT_LT(second.revision(), document.revision());
    EXPECT_FALSE(document.saveSnapshot(pendingWithoutChanges, utf8(destination), &error));
    EXPECT_FALSE(document.saveSnapshot(first, utf8(destination), &error));
    EXPECT_FALSE(document.saveSnapshot(second, utf8(destination), &error));
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(document.saveSnapshot(original, utf8(root), &error)) << error;
    EXPECT_FALSE(document.isDirty());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(3)).changed());
    const auto acceptedPreview = document.snapshot();
    ASSERT_TRUE(editor().commitClipEffectsTransaction().changed());
    EXPECT_FALSE(document.saveSnapshot(acceptedPreview, utf8(destination), &error));
    ASSERT_TRUE(document.saveSnapshot(original, utf8(root), &error)) << error;
    EXPECT_TRUE(document.isDirty());
    ASSERT_TRUE(document.save(&error)) << error;
    EXPECT_FALSE(document.isDirty());
}

TEST_F(GradeTransactionTest, NulloptEmptyAndOpaquePayloadsRemainDistinctAndRoundTrip) {
    const auto before = encoded();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(std::nullopt).changed());
    EXPECT_FALSE(clip().effects.has_value());
    const auto revision = document.revision();
    EXPECT_EQ(editor().updateClipEffectsTransaction(std::nullopt).status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision);
    ASSERT_TRUE(editor().updateClipEffectsTransaction(JsonValue::array()).changed());
    ASSERT_TRUE(clip().effects.has_value());
    EXPECT_TRUE(clip().effects->empty());
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_EQ(encoded(), before);
    ASSERT_TRUE(editor().setClipEffects("clip", std::nullopt).changed());
    const auto noEffects = encoded();
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(originalEffects()).changed());
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_EQ(encoded(), noEffects);
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    ASSERT_TRUE(editor().updateClipEffectsTransaction(JsonValue::array()).changed());
    ASSERT_TRUE(editor().commitClipEffectsTransaction().changed());
    ASSERT_TRUE(editor().undo());
    EXPECT_FALSE(clip().effects.has_value());
    ASSERT_TRUE(editor().redo());
    ASSERT_TRUE(clip().effects.has_value());
    EXPECT_TRUE(clip().effects->empty());
}

TEST_F(GradeTransactionTest, InvalidRequestsDoNotChangeRevisionHistoryOrPendingPreview) {
    const auto revision = document.revision();
    EXPECT_EQ(editor().updateClipEffectsTransaction(originalEffects()).status, EditStatus::Invalid);
    EXPECT_EQ(editor().commitClipEffectsTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(editor().cancelClipEffectsTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(editor().beginClipEffectsTransaction("missing").status, EditStatus::Invalid);
    EXPECT_EQ(editor().beginClipEffectsTransaction("sound").status, EditStatus::Unsupported);
    EXPECT_EQ(document.revision(), revision);
    EXPECT_FALSE(editor().undo());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    for (const auto& invalid : {JsonValue(nullptr), JsonValue::object(), JsonValue(4), JsonValue("bad")}) {
        EXPECT_EQ(editor().updateClipEffectsTransaction(invalid).status, EditStatus::Invalid);
        EXPECT_EQ(document.revision(), revision);
        EXPECT_EQ(clip().effects->dump(), originalEffects().dump());
    }
    ASSERT_TRUE(editor().updateClipEffectsTransaction(previewEffects(1)).changed());
    EXPECT_EQ(editor().updateClipEffectsTransaction(JsonValue::object()).status, EditStatus::Invalid);
    EXPECT_EQ(clip().effects->dump(), previewEffects(1).dump());
    ASSERT_TRUE(editor().cancelClipEffectsTransaction().changed());
    EXPECT_FALSE(editor().undo());
    EXPECT_FALSE(document.isDirty());
}
