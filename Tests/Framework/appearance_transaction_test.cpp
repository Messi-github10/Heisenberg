#include <Project/VideoProject.hpp>
#include <Models/ProjectCodec.hpp>
#include <gtest/gtest.h>
#include <filesystem>

namespace {
using namespace heisenberg;

class AppearanceTransactionTest : public testing::Test {
protected:
    VideoProject document;
    std::string error;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("heisenberg-appearance-transaction-" + makeUuid() + ".berg");
    std::string path() const {
        const auto value = root.u8string();
        return {reinterpret_cast<const char*>(value.c_str()), value.size()};
    }
    EditorViewModel& editor() { return document.editor(); }
    const Clip& clip() const { return *document.editor().activeTimeline()->clipById("clip"); }
    std::string encoded() const { return ProjectCodec::encode(document.editor().project()); }
    ClipAppearanceEdit values(double x, int64_t frame = 5) {
        ClipAppearanceEdit edit;
        edit.property = AppearanceProperty::Position;
        edit.localFrame = frame;
        edit.patch.centerX = x;
        return edit;
    }
    void SetUp() override {
        Clip clip;
        clip.id = "clip";
        clip.mediaRef = "media";
        clip.durationFrames = 30;
        clip.transform.rotationX = 10;
        clip.positionTrack = KeyframeTrackPayload{JsonValue::parse(R"({"opaque":{"keep":true},"keyframes":[
            {"frame":0,"value":{"a":0.0,"b":0.25,"future":7},"interpolationOut":"linear","vendor":1},
            {"frame":10,"value":{"a":1.0,"b":0.75},"interpolationOut":"hold"},
            {"frame":100,"value":{"a":0.5,"b":0.5}}]})")};
        clip.scaleTrack = KeyframeTrackPayload{JsonValue::object()}; // Unrelated malformed track.
        clip.effects = JsonValue::parse(R"([{"type":"future","opaque":[3,2,1]}])");
        Track track;
        track.id = "video";
        track.clips = {clip};
        Timeline timeline;
        timeline.id = "timeline";
        timeline.tracks = {track};
        ProjectFile project;
        project.timelines = {timeline};
        project.activeTimelineId = timeline.id;
        MediaManifest manifest;
        MediaManifestEntry entry;
        entry.id = "media";
        entry.duration = 10;
        entry.source = {MediaSource::Kind::External, "C:/offline-appearance-transaction.mp4"};
        ASSERT_TRUE(manifest.add(entry, &error)) << error;
        ASSERT_TRUE(editor().replaceDocument(project, manifest, {}, &error)) << error;
        ASSERT_TRUE(document.saveAs(path(), &error)) << error;
        ASSERT_FALSE(document.isDirty());
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST_F(AppearanceTransactionTest, IncrementalPreviewsCommitOneHistoryEntryAndCancelRestoresExactPayload) {
    const auto before = encoded();
    const auto revision = document.revision();
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    EXPECT_EQ(document.revision(), revision);
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.7)).changed());
    EXPECT_EQ(document.revision(), revision + 1);
    EXPECT_FALSE(document.isDirty());
    EXPECT_FALSE(editor().timelines().canUndo());
    auto sibling = values(0.7);
    sibling.patch = {};
    sibling.patch.centerY = 0.6;
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(sibling).changed());
    ClipAppearancePropertyState state;
    ASSERT_TRUE(inspectClipAppearanceProperty(clip(), AppearanceProperty::Position, 5, state));
    EXPECT_DOUBLE_EQ(*state.sampled.centerX, 0.7);
    EXPECT_DOUBLE_EQ(*state.sampled.centerY, 0.6);
    ASSERT_TRUE(editor().commitClipAppearanceTransaction().changed());
    EXPECT_EQ(document.revision(), revision + 3);
    EXPECT_TRUE(document.isDirty());
    const auto after = encoded();
    ASSERT_TRUE(editor().undo());
    EXPECT_EQ(encoded(), before);
    EXPECT_FALSE(editor().undo());
    ASSERT_TRUE(editor().redo());
    EXPECT_EQ(encoded(), after);
    ASSERT_TRUE(document.save(&error)) << error;
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.9)).changed());
    const auto previewRevision = document.revision();
    ASSERT_TRUE(editor().cancelClipAppearanceTransaction().changed());
    EXPECT_EQ(document.revision(), previewRevision + 1);
    EXPECT_EQ(encoded(), after);
    EXPECT_FALSE(document.isDirty());
}

TEST_F(AppearanceTransactionTest, ReturnToInitialSampleRestoresOriginalPayloadAndRetainsRedo) {
    ASSERT_TRUE(editor().setTrackHidden("video", true).changed());
    ASSERT_TRUE(editor().undo());
    ASSERT_TRUE(document.save(&error)) << error;
    const auto before = encoded();
    const auto revision = document.revision();
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.8)).changed());
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.5)).changed());
    EXPECT_EQ(encoded(), before);
    EXPECT_EQ(editor().commitClipAppearanceTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision + 2);
    EXPECT_FALSE(document.isDirty());
    EXPECT_TRUE(editor().redo());
}

TEST_F(AppearanceTransactionTest, NoOpSingleEditsAndTransactionsDoNotClearRedoOrChangeRevision) {
    ASSERT_TRUE(editor().setTrackHidden("video", true).changed());
    ASSERT_TRUE(editor().undo());
    const auto revision = document.revision();
    EXPECT_EQ(editor().editClipAppearance("clip", values(0.5)).status, EditStatus::NoOp);
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    EXPECT_EQ(editor().updateClipAppearanceTransaction(values(0.5)).status, EditStatus::NoOp);
    EXPECT_EQ(editor().commitClipAppearanceTransaction().status, EditStatus::NoOp);
    EXPECT_EQ(document.revision(), revision);
    EXPECT_TRUE(editor().redo());
}

TEST_F(AppearanceTransactionTest, SharedGuardPinsFrameAndBlocksEffectsHistoryMutationsAndPersistence) {
    ASSERT_TRUE(editor().setTrackHidden("video", true).changed());
    ASSERT_TRUE(editor().undo());
    const auto original = document.snapshot();
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    EXPECT_TRUE(editor().hasPendingDocumentTransaction());
    const auto pending = document.snapshot();
    EXPECT_EQ(editor().updateClipAppearanceTransaction(values(0.7, 6)).status, EditStatus::Invalid);
    EXPECT_EQ(editor().beginClipAppearanceTransaction("clip", 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor().beginClipEffectsTransaction("clip").status, EditStatus::Invalid);
    EXPECT_EQ(editor().setClipEffects("clip", std::nullopt).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setClipAppearance("clip", {}).status, EditStatus::Invalid);
    EXPECT_EQ(editor().editClipAppearance("clip", values(0.7)).status, EditStatus::Invalid);
    EXPECT_EQ(editor().moveClip("clip", 3).status, EditStatus::Invalid);
    EXPECT_EQ(editor().setTrackHidden("video", true).status, EditStatus::Invalid);
    EXPECT_FALSE(editor().undo());
    EXPECT_FALSE(editor().redo());
    EXPECT_FALSE(editor().importMedia("missing.mp4", &error));
    EXPECT_FALSE(editor().replaceDocument(original.project(), original.manifest(), {}, &error));
    EXPECT_FALSE(document.save(&error));
    EXPECT_FALSE(document.saveSnapshot(original, path(), &error));
    EXPECT_FALSE(document.loadPackage(path(), &error));
    EXPECT_EQ(editor().cancelClipAppearanceTransaction().status, EditStatus::NoOp);
    EXPECT_FALSE(editor().hasPendingDocumentTransaction());
    EXPECT_FALSE(document.saveSnapshot(pending, path(), &error));
    ASSERT_TRUE(editor().redo());
    ASSERT_TRUE(editor().beginClipEffectsTransaction("clip").changed());
    EXPECT_EQ(editor().beginClipAppearanceTransaction("clip", 5).status, EditStatus::Invalid);
    EXPECT_EQ(editor().editClipAppearance("clip", values(0.7)).status, EditStatus::Invalid);
    EXPECT_EQ(editor().cancelClipAppearanceTransaction().status, EditStatus::NoOp);
    EXPECT_TRUE(editor().hasPendingDocumentTransaction());
    EXPECT_EQ(editor().cancelClipEffectsTransaction().status, EditStatus::NoOp);
}

TEST_F(AppearanceTransactionTest, PreviewSnapshotsRemainTransientAfterCommitAndIndependentOfLaterPreviews) {
    const auto original = document.snapshot();
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.7)).changed());
    const auto preview = document.snapshot();
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(values(0.8)).changed());
    ASSERT_TRUE(editor().commitClipAppearanceTransaction().changed());
    ClipAppearancePropertyState state;
    ASSERT_TRUE(inspectClipAppearanceProperty(*preview.activeTimeline()->clipById("clip"), AppearanceProperty::Position, 5, state));
    EXPECT_DOUBLE_EQ(*state.sampled.centerX, 0.7);
    EXPECT_FALSE(document.saveSnapshot(preview, path(), &error));
    ASSERT_TRUE(document.saveSnapshot(original, path(), &error)) << error;
    EXPECT_TRUE(document.isDirty());
    ASSERT_TRUE(document.save(&error)) << error;
    EXPECT_FALSE(document.isDirty());
    VideoProject loaded;
    ASSERT_TRUE(loaded.loadPackage(path(), &error)) << error;
    EXPECT_EQ(*loaded.editor().activeTimeline()->clipById("clip"), clip());
}

TEST_F(AppearanceTransactionTest, ConstantsOutsideRangeAndExplicitToggleAreRetainedAndMalformedPropertyIsSafe) {
    ClipAppearanceEdit constant;
    constant.property = AppearanceProperty::Opacity;
    constant.localFrame = -10;
    constant.patch.opacity = 0.7;
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", -10).changed());
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(constant).changed());
    ASSERT_TRUE(editor().commitClipAppearanceTransaction().changed());
    EXPECT_DOUBLE_EQ(clip().opacity, 0.7);
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    auto toggle = values(0.5);
    toggle.action = AppearanceEditAction::ToggleKeyframe;
    toggle.patch = {};
    ASSERT_TRUE(editor().updateClipAppearanceTransaction(toggle).changed());
    EXPECT_EQ(editor().updateClipAppearanceTransaction(values(0.5)).status, EditStatus::NoOp);
    ASSERT_TRUE(editor().commitClipAppearanceTransaction().changed());
    EXPECT_EQ(clip().positionTrack->value["keyframes"].size(), 4);
    ASSERT_TRUE(editor().beginClipAppearanceTransaction("clip", 5).changed());
    auto bad = values(0.5);
    bad.property = static_cast<AppearanceProperty>(999);
    EXPECT_EQ(editor().updateClipAppearanceTransaction(bad).status, EditStatus::Invalid);
    EXPECT_EQ(editor().cancelClipAppearanceTransaction().status, EditStatus::NoOp);
}
} // namespace
