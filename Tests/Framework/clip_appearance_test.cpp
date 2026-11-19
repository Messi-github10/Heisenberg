#include <Editor/EditorViewModel.hpp>
#include <Models/ClipAppearance.hpp>
#include <Models/ProjectCodec.hpp>
#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <vector>

namespace {
using namespace heisenberg;

class ClipAppearanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        Clip clip;
        clip.id = "clip";
        clip.mediaRef = "video";
        clip.durationFrames = 30;
        clip.transform.rotationX = 12.0;
        clip.transform.rotationY = -24.0;
        clip.effects = JsonValue::parse(R"([{"type":"future.effect","payload":{"opaque":[1,2,3]}}])");
        const auto track = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[{"frame":2,"value":0.4}],"opaque":"keep"})")};
        clip.opacityTrack = clip.positionTrack = clip.scaleTrack = clip.rotationTrack = clip.cropTrack = clip.volumeTrack = track;
        Track video;
        video.id = "video-track";
        video.clips = {clip};
        Track audio;
        audio.id = "audio-track";
        audio.type = ClipType::Audio;
        Clip sound;
        sound.id = "sound";
        sound.mediaRef = "audio";
        sound.mediaType = sound.sourceClipType = ClipType::Audio;
        audio.clips = {sound};
        Timeline timeline;
        timeline.id = "timeline";
        timeline.tracks = {video, audio};
        project.timelines = {timeline};
        project.activeTimelineId = timeline.id;
        for (const auto type : {ClipType::Video, ClipType::Audio}) {
            MediaManifestEntry entry;
            entry.id = type == ClipType::Video ? "video" : "audio";
            entry.type = type;
            entry.duration = 10.0;
            entry.source = {MediaSource::Kind::External, "C:/offline-appearance.media"};
            ASSERT_TRUE(manifest.add(entry));
        }
        ASSERT_TRUE(editor.replaceDocument(project, manifest));
    }
    const Clip& clip() const { return *editor.activeTimeline()->clipById("clip"); }
    EditorViewModel editor;
    ProjectFile project;
    MediaManifest manifest;
};

TEST_F(ClipAppearanceTest, AllFieldsCommitAsOneUndoAndPreserveOpaquePayloads) {
    const auto before = editor.project();
    auto expected = clip();
    ClipAppearancePatch patch;
    patch.centerX = expected.transform.centerX = -0.4;
    patch.centerY = expected.transform.centerY = 1.4;
    patch.width = expected.transform.width = 0.75;
    patch.height = expected.transform.height = 1.25;
    patch.rotation = expected.transform.rotation = 765.0;
    patch.flipHorizontal = expected.transform.flipHorizontal = true;
    patch.flipVertical = expected.transform.flipVertical = true;
    patch.cropLeft = expected.crop.left = 0.1;
    patch.cropTop = expected.crop.top = 0.2;
    patch.cropRight = expected.crop.right = 0.3;
    patch.cropBottom = expected.crop.bottom = 0.4;
    patch.edgeRounding = expected.edgeRounding = 0.15;
    patch.edgeSoftness = expected.edgeSoftness = 0.25;
    patch.opacity = expected.opacity = 0.35;
    const auto revision = editor.revision();
    const auto result = editor.setClipAppearance("clip", patch);
    ASSERT_TRUE(result.changed()) << result.message;
    EXPECT_EQ(result.affectedIds, std::vector<std::string>{"clip"});
    EXPECT_EQ(editor.revision(), revision + 1);
    EXPECT_EQ(clip(), expected);
    EXPECT_EQ(editor.activeTimeline()->tracks[1], before.timelines[0].tracks[1]);
    const auto after = editor.project();
    EXPECT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    EXPECT_FALSE(editor.undo());
    EXPECT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
    EXPECT_FALSE(editor.redo());
}

TEST_F(ClipAppearanceTest, OmittedFieldsAndUntouchedImportedValuesRemainExact) {
    auto& imported = project.timelines[0].tracks[0].clips[0];
    imported.transform.centerX = 1.234567890123456e200;
    imported.transform.width = -3.0;
    imported.crop.left = 2.0;
    imported.edgeRounding = -0.2;
    ASSERT_TRUE(editor.replaceDocument(project, manifest));
    auto expected = clip();
    ClipAppearancePatch patch;
    patch.opacity = expected.opacity = 0.625;
    ASSERT_TRUE(editor.setClipAppearance("clip", patch).changed());
    EXPECT_EQ(clip(), expected);
}

TEST_F(ClipAppearanceTest, EmptyAndIdenticalPatchesKeepRevisionAndRedo) {
    ClipAppearancePatch change;
    change.rotation = 25.0;
    ASSERT_TRUE(editor.setClipAppearance("clip", change).changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto revision = editor.revision();
    EXPECT_EQ(editor.setClipAppearance("clip", {}).status, EditStatus::NoOp);
    ClipAppearancePatch identical;
    identical.centerX = clip().transform.centerX;
    identical.opacity = clip().opacity;
    identical.flipHorizontal = false;
    EXPECT_EQ(editor.setClipAppearance("clip", identical).status, EditStatus::NoOp);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST_F(ClipAppearanceTest, InvalidFieldsRejectTheWholeCandidateAndPreserveHistory) {
    ClipAppearancePatch change;
    change.opacity = 0.5;
    ASSERT_TRUE(editor.setClipAppearance("clip", change).changed());
    const auto after = editor.project();
    ASSERT_TRUE(editor.undo());
    const auto before = editor.project();
    const auto revision = editor.revision();
    using Field = std::optional<double> ClipAppearancePatch::*;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::vector<std::pair<Field, double>> invalid{
        {&ClipAppearancePatch::centerX, nan}, {&ClipAppearancePatch::centerY, infinity},
        {&ClipAppearancePatch::width, 0}, {&ClipAppearancePatch::width, -1},
        {&ClipAppearancePatch::height, infinity}, {&ClipAppearancePatch::rotation, nan},
        {&ClipAppearancePatch::cropLeft, -0.1}, {&ClipAppearancePatch::cropTop, 1.1},
        {&ClipAppearancePatch::cropRight, nan}, {&ClipAppearancePatch::cropBottom, infinity},
        {&ClipAppearancePatch::edgeRounding, -0.1}, {&ClipAppearancePatch::edgeSoftness, 1.1},
        {&ClipAppearancePatch::opacity, nan}, {&ClipAppearancePatch::opacity, -0.1},
        {&ClipAppearancePatch::opacity, 1.1}
    };
    for (const auto& [field, value] : invalid) {
        ClipAppearancePatch patch;
        patch.centerX = 0.75;
        patch.*field = value;
        EXPECT_EQ(editor.setClipAppearance("clip", patch).status, EditStatus::Invalid);
        EXPECT_EQ(editor.project(), before);
        EXPECT_EQ(editor.revision(), revision);
    }
    ClipAppearancePatch crop;
    crop.cropLeft = 0.6;
    crop.cropRight = 0.4;
    EXPECT_EQ(editor.setClipAppearance("clip", crop).status, EditStatus::Invalid);
    crop.cropLeft = 0.0;
    crop.cropRight = 0.0;
    crop.cropTop = 0.5;
    crop.cropBottom = 0.5;
    EXPECT_EQ(editor.setClipAppearance("clip", crop).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), before);
    EXPECT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

TEST_F(ClipAppearanceTest, CropValidationUsesAllMergedSides) {
    auto& imported = project.timelines[0].tracks[0].clips[0];
    imported.crop.left = 0.8;
    ASSERT_TRUE(editor.replaceDocument(project, manifest));
    ClipAppearancePatch patch;
    patch.cropRight = 0.2;
    EXPECT_EQ(editor.setClipAppearance("clip", patch).status, EditStatus::Invalid);
    patch.cropLeft = 0.1;
    EXPECT_TRUE(editor.setClipAppearance("clip", patch).changed());
    imported.crop.bottom = 1.2;
    ASSERT_TRUE(editor.replaceDocument(project, manifest));
    patch = {};
    patch.cropLeft = 0.1;
    EXPECT_EQ(editor.setClipAppearance("clip", patch).status, EditStatus::Invalid);
}

TEST_F(ClipAppearanceTest, MissingAudioAndPendingEffectsAreRejectedWithoutMutation) {
    const auto before = editor.project();
    ClipAppearancePatch patch;
    patch.width = 2.0;
    EXPECT_EQ(editor.setClipAppearance("missing", patch).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipAppearance("sound", patch).status, EditStatus::Unsupported);
    ASSERT_TRUE(editor.beginClipEffectsTransaction("clip").message.empty());
    EXPECT_EQ(editor.setClipAppearance("clip", patch).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipAppearance("clip", {}).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), before);
    ASSERT_TRUE(editor.cancelClipEffectsTransaction().message.empty());
    EXPECT_TRUE(editor.setClipAppearance("clip", patch).changed());
}

TEST(ConstantGeometryTest, ValidatesGeometrySeparatelyFromOpacityTiltAndAutomation) {
    Clip clip;
    clip.transform.centerX = -1e200;
    clip.transform.centerY = 1e200;
    clip.transform.width = 1e200;
    clip.transform.rotation = -1e200;
    clip.transform.rotationX = std::numeric_limits<double>::quiet_NaN();
    clip.transform.rotationY = std::numeric_limits<double>::infinity();
    clip.opacity = -1.0;
    clip.positionTrack = KeyframeTrackPayload{JsonValue{{"arbitrary", true}}};
    EXPECT_TRUE(validateConstantGeometry(clip));
    const std::vector<std::function<void(Clip&)>> invalid{
        [](Clip& c) { c.transform.centerX = std::numeric_limits<double>::infinity(); },
        [](Clip& c) { c.transform.centerY = std::numeric_limits<double>::quiet_NaN(); },
        [](Clip& c) { c.transform.width = 0; },
        [](Clip& c) { c.transform.height = -0.1; },
        [](Clip& c) { c.transform.rotation = std::numeric_limits<double>::infinity(); },
        [](Clip& c) { c.crop.left = -0.1; }, [](Clip& c) { c.crop.top = 1.1; },
        [](Clip& c) { c.crop.right = std::numeric_limits<double>::quiet_NaN(); },
        [](Clip& c) { c.crop.bottom = std::numeric_limits<double>::infinity(); },
        [](Clip& c) { c.crop.left = 0.6; c.crop.right = 0.4; },
        [](Clip& c) { c.crop.top = 0.4; c.crop.bottom = 0.6; },
        [](Clip& c) { c.edgeRounding = -0.1; }, [](Clip& c) { c.edgeSoftness = 1.1; }
    };
    for (const auto& mutate : invalid) {
        auto candidate = clip;
        mutate(candidate);
        std::string error;
        EXPECT_FALSE(validateConstantGeometry(candidate, &error));
        EXPECT_FALSE(error.empty());
    }
}
} // namespace
