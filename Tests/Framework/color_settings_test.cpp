#include <Compositing/CompositionBuilder.hpp>
#include <Models/ProjectCodec.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>
#include <array>

namespace {
using namespace heisenberg;
ProjectFile colorProject() {
    ProjectFile project;
    Timeline timeline; timeline.id = "timeline";
    Track track; track.id = "video";
    Clip clip; clip.id = "clip"; clip.mediaRef = "media"; clip.durationFrames = 20;
    clip.trimEndFrame = 20;
    track.clips.push_back(clip); timeline.tracks.push_back(track);
    project.timelines.push_back(timeline); project.activeTimelineId = timeline.id;
    return project;
}
MediaManifest colorManifest() {
    MediaManifest manifest;
    MediaManifestEntry entry; entry.id = "media"; entry.duration = 10;
    entry.source = {MediaSource::Kind::External, "C:/offline.mp4"};
    EXPECT_TRUE(manifest.add(entry)); return manifest;
}
JsonValue gradeTrack() {
    return JsonValue::parse(R"([{"id":"basic-grade","type":"video.basicGrade","enabled":true,"params":{"exposure":{"value":0,"track":{"keyframes":[{"frame":0,"value":0,"interpolationOut":"linear"},{"frame":19,"value":2,"interpolationOut":"hold"}]}},"contrast":{"value":1},"saturation":{"value":1}}}])");
}
}

TEST(ColorSettingsTest, NewProjectsUseFinishedSdrDefaults) {
    const auto settings = colorProject().colorSettings;
    EXPECT_EQ(settings.schemaVersion, 1);
    EXPECT_EQ(settings.inputPolicyVersion, 2);
    EXPECT_EQ(settings.semanticsVersion, 2);
    EXPECT_EQ(settings.previewOutput, (OutputSelection{"sRGB - Display", "Un-tone-mapped"}));
    EXPECT_EQ(settings.exportOutput, (OutputSelection{"Rec.1886 Rec.709 - Display", "Un-tone-mapped"}));
    EXPECT_FALSE(settings.legacyDefaultsApplied);
    EXPECT_TRUE(settings.inputColorSpaces.empty());
    EXPECT_TRUE(supportedColorSettings(settings));
    ProjectFile reopened; std::string error;
    ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(colorProject()), reopened, &error)) << error;
    EXPECT_EQ(reopened.colorSettings, settings);
}

TEST(ColorSettingsTest, ExplicitV1CompleteExtensionPreservesSavedAppearance) {
    auto project = colorProject();
    project.colorSettings = legacyColorSettings();
    project.colorSettings.inputColorSpaces["media"] = "Camera Rec.709";
    project.colorSettings.unknownFields["futureProperty"] = 12;
    const auto root = JsonValue::parse(ProjectCodec::encode(project));
    ProjectFile decoded; std::string error;
    ASSERT_TRUE(ProjectCodec::decode(root.dump(), decoded, &error)) << error;
    EXPECT_EQ(decoded.colorSettings, project.colorSettings);
    EXPECT_FALSE(decoded.colorSettings.legacyDefaultsApplied);
    EXPECT_EQ(JsonValue::parse(ProjectCodec::encode(decoded)), root);
    // Explicit saved selections and an earlier migration marker must not be normalized on reopen.
    project.colorSettings.previewOutput.view = "Un-tone-mapped";
    project.colorSettings.legacyDefaultsApplied = true;
    ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(project), decoded, &error)) << error;
    EXPECT_EQ(decoded.colorSettings, project.colorSettings);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(decoded.timelines.front(), colorManifest(), {}, decoded.colorSettings, plan, &error)) << error;
    ASSERT_EQ(plan.warnings.size(), 1u);
    EXPECT_NE(plan.warnings.front().find("saved legacy defaults marker preserved"), std::string::npos);
    EXPECT_EQ(plan.warnings.front().find("ordinary SDR defaults"), std::string::npos);
}

TEST(ColorSettingsTest, ExplicitV1MissingOutputFieldsKeepsLegacyDefaults) {
    auto root = JsonValue::parse(ProjectCodec::encode(colorProject()));
    auto& color = root["extensions"]["org.heisenberg.color"];
    color = {{"schemaVersion", 1}, {"inputPolicyVersion", 1}, {"semanticsVersion", 1},
             {"inputColorSpaces", {{"media", "ACEScg"}}}};
    ProjectFile decoded; std::string error;
    ASSERT_TRUE(ProjectCodec::decode(root.dump(), decoded, &error)) << error;
    auto expected = legacyColorSettings(); expected.inputColorSpaces["media"] = "ACEScg";
    EXPECT_EQ(decoded.colorSettings, expected);
    // Missing fields inside a saved output also use the legacy defaults, not new-project defaults.
    color["previewOutput"] = {{"display", "sRGB - Display"}};
    color["exportOutput"] = {{"view", "ACES 2.0 - SDR 100 nits (Rec.709)"}};
    ASSERT_TRUE(ProjectCodec::decode(root.dump(), decoded, &error)) << error;
    EXPECT_EQ(decoded.colorSettings, expected);
    // An explicit extension with omitted versions is still legacy, not an extension-less package.
    color = JsonValue::object();
    ASSERT_TRUE(ProjectCodec::decode(root.dump(), decoded, &error)) << error;
    EXPECT_EQ(decoded.colorSettings, legacyColorSettings());
    ProjectFile reopened;
    ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(decoded), reopened, &error)) << error;
    EXPECT_EQ(reopened.colorSettings, decoded.colorSettings);
}

TEST(ColorSettingsTest, ExplicitV2MissingOutputsUsesFinishedSdrDefaults) {
    ColorSettings decoded; std::string error;
    ASSERT_TRUE(decodeColorSettings({{"inputPolicyVersion", 2}, {"semanticsVersion", 2}}, decoded, &error)) << error;
    EXPECT_EQ(decoded, ColorSettings{});
}

TEST(ColorSettingsTest, MissingExtensionMigratesToSdrAndUnknownExtensionFieldsSurviveRoundTrip) {
    auto root = JsonValue::parse(ProjectCodec::encode(colorProject()));
    root.erase("extensions");
    root["foreignRoot"] = {{"future", 3}};
    ProjectFile decoded;
    std::string error;
    ASSERT_TRUE(ProjectCodec::decode(root.dump(), decoded, &error)) << error;
    EXPECT_TRUE(decoded.colorSettings.legacyDefaultsApplied);
    auto expected = ColorSettings{}; expected.legacyDefaultsApplied = true;
    EXPECT_EQ(decoded.colorSettings, expected);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(decoded.timelines.front(), colorManifest(), {}, decoded.colorSettings, plan, &error)) << error;
    ASSERT_EQ(plan.warnings.size(), 1u);
    EXPECT_NE(plan.warnings.front().find("ordinary SDR defaults"), std::string::npos);
    EXPECT_NE(plan.warnings.front().find("input policy v2"), std::string::npos);
    auto encoded = JsonValue::parse(ProjectCodec::encode(decoded));
    encoded["extensions"]["foreign.namespace"] = {{"opaque", true}};
    encoded["extensions"]["org.heisenberg.color"]["futureProperty"] = {{"enabled", true}};
    encoded["extensions"]["org.heisenberg.color"]["previewOutput"]["futureSelection"] = 12;
    ASSERT_TRUE(ProjectCodec::decode(encoded.dump(), decoded, &error)) << error;
    EXPECT_EQ(nlohmann::json::parse(ProjectCodec::encode(decoded)), nlohmann::json::parse(encoded.dump()));
    ProjectFile reopened;
    ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(decoded), reopened, &error)) << error;
    EXPECT_EQ(decoded, reopened);
}

TEST(ColorSettingsTest, FutureVersionsArePreservedAndDiagnosedWithoutGuessing) {
    auto project = colorProject(); project.colorSettings.schemaVersion = 9;
    ProjectFile decoded; std::string error;
    ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(project), decoded, &error)) << error;
    EXPECT_EQ(decoded.colorSettings.schemaVersion, 9);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(decoded.timelines.front(), colorManifest(), {}, decoded.colorSettings, plan, &error)) << error;
    ASSERT_FALSE(plan.warnings.empty());
    EXPECT_NE(plan.warnings.front().find("unsupported color settings version"), std::string::npos);
}

TEST(ColorSettingsTest, VersionGateAcceptsOnlyKnownVersionPairsAndPreservesFutureSettings) {
    EXPECT_TRUE(supportedColorSettings(legacyColorSettings()));
    EXPECT_TRUE(supportedColorSettings(ColorSettings{}));
    for (const auto versions : {std::array{1, 1, 2}, std::array{1, 2, 1}, std::array{1, 3, 3}, std::array{2, 2, 2}}) {
        auto project = colorProject();
        project.colorSettings.schemaVersion = versions[0];
        project.colorSettings.inputPolicyVersion = versions[1];
        project.colorSettings.semanticsVersion = versions[2];
        EXPECT_TRUE(validateColorSettings(project.colorSettings));
        EXPECT_FALSE(supportedColorSettings(project.colorSettings));
        ProjectFile decoded; std::string error;
        ASSERT_TRUE(ProjectCodec::decode(ProjectCodec::encode(project), decoded, &error)) << error;
        EXPECT_EQ(decoded.colorSettings, project.colorSettings);
        CompositionPlan plan;
        ASSERT_TRUE(CompositionBuilder::build(decoded.timelines.front(), colorManifest(), {}, decoded.colorSettings, plan, &error)) << error;
        ASSERT_EQ(plan.warnings.size(), 1u);
        EXPECT_NE(plan.warnings.front().find("cannot be executed"), std::string::npos);
        EditorViewModel editor;
        ASSERT_TRUE(editor.replaceDocument(decoded, colorManifest(), {}, &error)) << error;
        const auto revision = editor.revision();
        EXPECT_EQ(editor.setColorSettings(decoded.colorSettings).status, EditStatus::Unsupported);
        EXPECT_EQ(editor.revision(), revision);
        EXPECT_EQ(editor.project(), decoded);
    }
}

TEST(ColorEditorTest, ExplicitModeUpgradeIsOneUndoPreservesOverridesAndNoOpKeepsRedo) {
    auto project = colorProject(); project.colorSettings = legacyColorSettings();
    project.colorSettings.inputColorSpaces["media"] = "Camera Rec.709";
    project.colorSettings.inputColorSpaces["unavailable-media"] = "preserved custom Log";
    project.colorSettings.unknownFields["future"] = true;
    EditorViewModel editor; std::string error;
    ASSERT_TRUE(editor.replaceDocument(project, colorManifest(), {}, &error)) << error;
    const auto revision = editor.revision();
    EXPECT_EQ(editor.setColorSettings(project.colorSettings).status, EditStatus::NoOp);
    auto settings = project.colorSettings;
    settings.inputPolicyVersion = 2; settings.semanticsVersion = 2;
    settings.previewOutput = ColorSettings{}.previewOutput;
    settings.exportOutput = ColorSettings{}.exportOutput;
    ASSERT_TRUE(editor.setColorSettings(settings).changed());
    EXPECT_EQ(editor.revision(), revision + 1);
    EXPECT_EQ(editor.project().colorSettings.inputColorSpaces, project.colorSettings.inputColorSpaces);
    EXPECT_EQ(editor.project().colorSettings.unknownFields, project.colorSettings.unknownFields);
    EXPECT_EQ(editor.setColorSettings(settings).status, EditStatus::NoOp);
    EXPECT_EQ(editor.revision(), revision + 1);
    ASSERT_TRUE(editor.undo()); EXPECT_EQ(editor.project(), project);
    EXPECT_EQ(editor.setColorSettings(project.colorSettings).status, EditStatus::NoOp);
    ASSERT_TRUE(editor.redo()); EXPECT_EQ(editor.project().colorSettings, settings);
    settings.previewOutput.view = "ACES 2.0 - SDR 100 nits (Rec.709)";
    settings.exportOutput.view = "ACES 2.0 - SDR 100 nits (Rec.709)";
    ASSERT_TRUE(editor.setColorSettings(settings).changed());
    EXPECT_EQ(editor.project().colorSettings.inputPolicyVersion, 2);
    EXPECT_EQ(editor.project().colorSettings.semanticsVersion, 2);
    EXPECT_EQ(editor.project().colorSettings.inputColorSpaces, project.colorSettings.inputColorSpaces);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project().colorSettings.previewOutput.view, "Un-tone-mapped");
}

TEST(ColorEditorTest, ColorAndEffectsShareOrderedUndoAndNoOpDoesNotClearRedo) {
    EditorViewModel editor; std::string error;
    ASSERT_TRUE(editor.replaceDocument(colorProject(), colorManifest(), {}, &error)) << error;
    const auto before = editor.project(); const auto revision = editor.revision();
    auto settings = before.colorSettings; settings.inputColorSpaces["media"] = "ACEScg";
    ASSERT_TRUE(editor.setColorSettings(settings).changed());
    EXPECT_EQ(editor.setColorSettings(settings).status, EditStatus::NoOp);
    ASSERT_TRUE(editor.setClipEffects("clip", gradeTrack()).changed());
    EXPECT_EQ(editor.revision(), revision + 2);
    ASSERT_TRUE(editor.undo());
    EXPECT_FALSE(editor.activeTimeline()->clipById("clip")->effects);
    ASSERT_TRUE(editor.undo()); EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.setColorSettings(before.colorSettings).status, EditStatus::NoOp);
    ASSERT_TRUE(editor.redo()); EXPECT_EQ(editor.project().colorSettings, settings);
    ASSERT_TRUE(editor.redo()); EXPECT_EQ(editor.activeTimeline()->clipById("clip")->effects, gradeTrack());
    EXPECT_EQ(editor.setMediaInputColorSpace("missing", "ACEScg").status, EditStatus::Invalid);
}

TEST(ColorEditorTest, SplitAndTrimRetainBracketingGradeKeysInClipLocalTime) {
    auto project = colorProject(); project.timelines[0].tracks[0].clips[0].effects = gradeTrack();
    EditorViewModel editor; std::string error;
    ASSERT_TRUE(editor.replaceDocument(project, colorManifest(), {}, &error)) << error;
    const auto split = editor.splitClip("clip", 8); ASSERT_TRUE(split.changed());
    const auto* right = editor.activeTimeline()->clipById(split.affectedIds[1]); ASSERT_NE(right, nullptr);
    const auto& rightKeys = (*right->effects)[0]["params"]["exposure"]["track"]["keyframes"];
    EXPECT_EQ(rightKeys[0]["frame"], -8); EXPECT_EQ(rightKeys[1]["frame"], 11);
    const auto& leftKeys = (*editor.activeTimeline()->clipById("clip")->effects)[0]["params"]["exposure"]["track"]["keyframes"];
    EXPECT_EQ(leftKeys[1]["frame"], 19); // Future anchor retains the pre-cut slope.
    ASSERT_TRUE(editor.undo());
    ASSERT_TRUE(editor.trimClip("clip", TrimEdge::Left, 5).changed());
    const auto& trimmed = (*editor.activeTimeline()->clipById("clip")->effects)[0]["params"]["exposure"]["track"]["keyframes"];
    EXPECT_EQ(trimmed[0]["frame"], -5); EXPECT_EQ(trimmed[1]["frame"], 14);
    ASSERT_TRUE(editor.trimClip("clip", TrimEdge::Left, -5).changed());
    EXPECT_EQ(editor.project(), project);
}

TEST(ColorSnapshotTest, CapturesColorEffectsAndLocalFrameWithoutBorrowingEditor) {
    VideoProject project; std::string error;
    ASSERT_TRUE(project.editor().replaceDocument(colorProject(), colorManifest(), {}, &error)) << error;
    ASSERT_TRUE(project.editor().setClipEffects("clip", gradeTrack()).changed());
    auto settings = project.editor().project().colorSettings; settings.inputColorSpaces["media"] = "ACEScg";
    ASSERT_TRUE(project.editor().setColorSettings(settings).changed());
    const auto snapshot = project.snapshot();
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(snapshot, plan, &error)) << error;
    ASSERT_TRUE(project.editor().setMediaInputColorSpace("media", "Raw").changed());
    ASSERT_TRUE(project.editor().setClipEffects("clip", std::nullopt).changed());
    EXPECT_EQ(plan.colorSettings.inputColorSpaces.at("media"), "ACEScg");
    EXPECT_EQ(plan.clips.front().visualSnapshot.effects, gradeTrack());
    ASSERT_TRUE(plan.videoSampleAt(7)); EXPECT_EQ(plan.videoSampleAt(7)->clipLocalFrame, 7);
    EXPECT_EQ(snapshot.project().colorSettings, settings);
}
