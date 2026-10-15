#include <Compositing/Grade/GradeCompiler.hpp>
#include <Editor/EditorViewModel.hpp>
#include <Models/ProjectFile.hpp>
#include <gtest/gtest.h>

namespace {
using namespace heisenberg;
using namespace heisenberg::grade;

Clip animatedClip(const std::string& interpolation) {
    Clip clip;
    clip.id = "animated";
    clip.mediaRef = "media";
    clip.startFrame = 10;
    clip.durationFrames = 24;
    clip.trimStartFrame = 3;
    clip.speed = 2.0; // Clip-local animation is unrelated to source time.
    clip.effects = JsonValue::parse(R"([{"id":"basic-grade","type":"video.basicGrade","enabled":true,"params":{"exposure":{"value":1,"track":{"keyframes":[{"frame":0,"value":1,"interpolationOut":"linear"},{"frame":12,"value":3,"interpolationOut":"linear"},{"frame":30,"value":2,"interpolationOut":"hold"}]}}}}])");
    (*clip.effects)[0]["params"]["exposure"]["track"]["keyframes"][0]["interpolationOut"] = interpolation;
    (*clip.effects)[0]["params"]["exposure"]["track"]["keyframes"][1]["interpolationOut"] = interpolation;
    return clip;
}

bool install(EditorViewModel& editor, const Clip& clip, std::string& error) {
    ProjectFile project;
    Timeline timeline;
    timeline.id = "timeline";
    timeline.fps = 30;
    Track track;
    track.id = "video";
    track.clips.push_back(clip);
    timeline.tracks.push_back(track);
    project.timelines.push_back(timeline);
    project.activeTimelineId = timeline.id;
    MediaManifest manifest;
    MediaManifestEntry entry;
    entry.id = "media";
    entry.name = "Media";
    entry.duration = 10;
    entry.source = {MediaSource::Kind::External, "C:/offline-grade.mp4"};
    if (!manifest.add(entry)) return false;
    return editor.replaceDocument(std::move(project), std::move(manifest), {}, &error);
}

float exposureAt(const Clip& clip, int64_t frame) {
    CompiledGrade compiled;
    std::string error;
    EXPECT_TRUE(GradeCompiler::compile(clip, frame, compiled, &error)) << error;
    if (compiled.stages.empty()) return 0.0f;
    return std::get<float>(compiled.stages[0].parameterUpdates[0].parameters.at("exposure"));
}

} // namespace

TEST(GradeEditContinuityTest, SplitRetainsHoldAndLinearSamplesIncludingCrossBoundaryAnchors) {
    for (const std::string interpolation : {"hold", "linear"}) {
        const auto original = animatedClip(interpolation);
        EditorViewModel editor;
        std::string error;
        ASSERT_TRUE(install(editor, original, error)) << error;
        ASSERT_TRUE(editor.splitClip(original.id, original.startFrame + 7).changed());
        const auto& clips = editor.activeTimeline()->tracks[0].clips;
        ASSERT_EQ(clips.size(), 2u);
        const auto& left = clips[0];
        const auto& right = clips[1];
        for (int64_t frame = 0; frame < 7; ++frame) {
            EXPECT_FLOAT_EQ(exposureAt(left, frame), exposureAt(original, frame)) << interpolation << " " << frame;
        }
        for (int64_t frame = 0; frame < original.durationFrames - 7; ++frame) {
            EXPECT_FLOAT_EQ(exposureAt(right, frame), exposureAt(original, frame + 7)) << interpolation << " " << frame;
        }
        EXPECT_LT((*right.effects)[0]["params"]["exposure"]["track"]["keyframes"][0]["frame"].get<int64_t>(), 0);
        EXPECT_TRUE(editor.undo());
        EXPECT_EQ(editor.activeTimeline()->tracks[0].clips[0], original);
        EXPECT_TRUE(editor.redo());
        EXPECT_FLOAT_EQ(exposureAt(editor.activeTimeline()->tracks[0].clips[1], 0), exposureAt(original, 7));
    }
}

TEST(GradeEditContinuityTest, LeftTrimAndRightTrimPreserveHoldLinearAndExtensionRestoresCurve) {
    for (const std::string interpolation : {"hold", "linear"}) {
        const auto original = animatedClip(interpolation);
        EditorViewModel editor;
        std::string error;
        ASSERT_TRUE(install(editor, original, error)) << error;
        ASSERT_TRUE(editor.trimClip(original.id, TrimEdge::Left, 5).changed());
        auto trimmed = editor.activeTimeline()->tracks[0].clips[0];
        for (int64_t frame = 0; frame < trimmed.durationFrames; ++frame) {
            EXPECT_FLOAT_EQ(exposureAt(trimmed, frame), exposureAt(original, frame + 5)) << interpolation << " " << frame;
        }
        ASSERT_TRUE(editor.trimClip(original.id, TrimEdge::Right, -4).changed());
        auto rightTrimmed = editor.activeTimeline()->tracks[0].clips[0];
        for (int64_t frame = 0; frame < rightTrimmed.durationFrames; ++frame) {
            EXPECT_FLOAT_EQ(exposureAt(rightTrimmed, frame), exposureAt(original, frame + 5));
        }
        ASSERT_TRUE(editor.trimClip(original.id, TrimEdge::Left, -5).changed());
        const auto& extended = editor.activeTimeline()->tracks[0].clips[0];
        for (int64_t frame = 0; frame < extended.durationFrames; ++frame) {
            EXPECT_FLOAT_EQ(exposureAt(extended, frame), exposureAt(original, frame));
        }
    }
}
