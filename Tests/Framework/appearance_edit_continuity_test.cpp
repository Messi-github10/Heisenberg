#include <Editor/EditorViewModel.hpp>
#include <Models/ClipAppearance.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace heisenberg;
using TrackField = std::optional<KeyframeTrackPayload> Clip::*;

const std::vector<TrackField> appearanceFields{
    &Clip::opacityTrack, &Clip::positionTrack, &Clip::scaleTrack,
    &Clip::rotationTrack, &Clip::cropTrack};

KeyframeTrackPayload animation(const std::string& interpolation, const std::vector<JsonValue>& values) {
    JsonValue keyframes = JsonValue::array();
    const std::vector<int64_t> frames{-6, 6, 15, 34};
    for (size_t i = 0; i < frames.size(); ++i) {
        keyframes.push_back(JsonValue{{"frame", frames[i]}, {"value", values[i]},
                                      {"interpolationOut", interpolation}});
    }
    return KeyframeTrackPayload{JsonValue{{"keyframes", std::move(keyframes)}}};
}

Clip animatedClip(const std::string& interpolation) {
    Clip clip;
    clip.id = "animated";
    clip.mediaRef = "media";
    clip.startFrame = 10;
    clip.durationFrames = 24;
    clip.trimStartFrame = 20;
    clip.trimEndFrame = 100;
    clip.speed = 2.0;
    clip.opacity = 0.9;
    clip.transform.centerX = 0.4;
    clip.transform.centerY = 0.6;
    clip.transform.width = 2.0;
    clip.transform.height = 0.75;
    clip.transform.rotation = 17.0;
    clip.transform.flipHorizontal = true;
    clip.edgeRounding = 0.12;
    clip.edgeSoftness = 0.08;
    clip.blendMode = "screen";
    clip.opacityTrack = animation(interpolation, {0.2, 0.8, 0.4, 0.6});
    clip.positionTrack = animation(interpolation, {
        JsonValue{{"a", -0.2}, {"b", 0.3}}, JsonValue{{"a", 0.7}, {"b", 0.4}},
        JsonValue{{"a", 0.1}, {"b", 0.9}}, JsonValue{{"a", 1.2}, {"b", -0.1}}});
    clip.scaleTrack = animation(interpolation, {
        JsonValue{{"a", 0.5}, {"b", 1.2}}, JsonValue{{"a", 1.5}, {"b", 0.8}},
        JsonValue{{"a", 0.8}, {"b", 1.6}}, JsonValue{{"a", 1.1}, {"b", 0.6}}});
    clip.rotationTrack = animation(interpolation, {-30.0, 45.0, 120.0, -10.0});
    clip.cropTrack = animation(interpolation, {
        JsonValue{{"left", 0.02}, {"top", 0.04}, {"right", 0.06}, {"bottom", 0.08}},
        JsonValue{{"left", 0.10}, {"top", 0.12}, {"right", 0.14}, {"bottom", 0.16}},
        JsonValue{{"left", 0.18}, {"top", 0.16}, {"right", 0.12}, {"bottom", 0.10}},
        JsonValue{{"left", 0.04}, {"top", 0.06}, {"right", 0.08}, {"bottom", 0.02}}});
    return clip;
}

bool install(EditorViewModel& editor, const Clip& clip, std::string& error,
             const std::vector<Clip>& additional = {}) {
    ProjectFile project;
    Timeline timeline;
    timeline.id = "timeline";
    timeline.fps = 30;
    Track track;
    track.id = "video";
    track.clips.push_back(clip);
    track.clips.insert(track.clips.end(), additional.begin(), additional.end());
    track.sortClips();
    Track other;
    other.id = "other-video";
    timeline.tracks = {track, other};
    project.timelines = {timeline};
    project.activeTimelineId = timeline.id;
    MediaManifest manifest;
    MediaManifestEntry entry;
    entry.id = "media";
    entry.name = "Offline animation";
    entry.duration = 30.0;
    entry.source = {MediaSource::Kind::External, "C:/offline-appearance-continuity.mp4"};
    if (!manifest.add(entry)) return false;
    return editor.replaceDocument(std::move(project), std::move(manifest), {}, &error);
}

void expectSamples(const Clip& actual, const Clip& original, int64_t originalOffset = 0) {
    std::string error;
    ASSERT_TRUE(validateClipAppearanceTracks(actual, &error)) << error;
    for (int64_t frame = 0; frame < actual.durationFrames; ++frame) {
        SCOPED_TRACE(frame);
        Clip sampled, expected;
        ASSERT_TRUE(sampleClipAppearance(actual, frame, sampled, &error)) << error;
        ASSERT_TRUE(sampleClipAppearance(original, frame + originalOffset, expected, &error)) << error;
        EXPECT_DOUBLE_EQ(sampled.opacity, expected.opacity);
        EXPECT_DOUBLE_EQ(sampled.transform.centerX, expected.transform.centerX);
        EXPECT_DOUBLE_EQ(sampled.transform.centerY, expected.transform.centerY);
        EXPECT_DOUBLE_EQ(sampled.transform.width, expected.transform.width);
        EXPECT_DOUBLE_EQ(sampled.transform.height, expected.transform.height);
        EXPECT_DOUBLE_EQ(sampled.transform.rotation, expected.transform.rotation);
        EXPECT_DOUBLE_EQ(sampled.crop.left, expected.crop.left);
        EXPECT_DOUBLE_EQ(sampled.crop.top, expected.crop.top);
        EXPECT_DOUBLE_EQ(sampled.crop.right, expected.crop.right);
        EXPECT_DOUBLE_EQ(sampled.crop.bottom, expected.crop.bottom);
        EXPECT_EQ(sampled.transform.flipHorizontal, expected.transform.flipHorizontal);
        EXPECT_DOUBLE_EQ(sampled.edgeRounding, expected.edgeRounding);
        EXPECT_DOUBLE_EQ(sampled.edgeSoftness, expected.edgeSoftness);
        EXPECT_EQ(sampled.blendMode, expected.blendMode);
    }
}

void expectAnchors(const Clip& clip) {
    for (const auto field : appearanceFields) {
        ASSERT_TRUE((clip.*field).has_value());
        const auto& keys = (clip.*field)->value.at("keyframes");
        ASSERT_EQ(keys.size(), 4u);
        EXPECT_LT(keys.front().at("frame").get<int64_t>(), 0);
        EXPECT_GT(keys.back().at("frame").get<int64_t>(), clip.durationFrames);
    }
}

void expectHistory(EditorViewModel& editor, const ProjectFile& before, const ProjectFile& after) {
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
}

struct TemporaryPackage {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("heisenberg-appearance-continuity-" + makeUuid() + ".berg");
    ~TemporaryPackage() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::string utf8() const {
        const auto value = path.u8string();
        return {reinterpret_cast<const char*>(value.c_str()), value.size()};
    }
};

class AppearanceEditContinuityTest : public ::testing::TestWithParam<std::string> {};

TEST_P(AppearanceEditContinuityTest, SplitPreservesEveryFrameAndOutOfRangeAnchorsThroughHistoryAndReopen) {
    const auto original = animatedClip(GetParam());
    VideoProject project;
    auto& editor = project.editor();
    std::string error;
    ASSERT_TRUE(install(editor, original, error)) << error;
    // Check that track values directly replace centers and width/height.
    Clip atKey;
    ASSERT_TRUE(sampleClipAppearance(original, 6, atKey, &error)) << error;
    EXPECT_DOUBLE_EQ(atKey.opacity, 0.8);
    EXPECT_DOUBLE_EQ(atKey.transform.centerX, 0.7);
    EXPECT_DOUBLE_EQ(atKey.transform.centerY, 0.4);
    EXPECT_DOUBLE_EQ(atKey.transform.width, 1.5);
    EXPECT_DOUBLE_EQ(atKey.transform.height, 0.8);
    EXPECT_DOUBLE_EQ(atKey.transform.rotation, 45.0);
    EXPECT_DOUBLE_EQ(atKey.crop.left, 0.10);
    EXPECT_DOUBLE_EQ(atKey.crop.top, 0.12);
    EXPECT_DOUBLE_EQ(atKey.crop.right, 0.14);
    EXPECT_DOUBLE_EQ(atKey.crop.bottom, 0.16);
    const auto before = editor.project();
    const auto result = editor.splitClip(original.id, original.startFrame + 9);
    ASSERT_TRUE(result.changed()) << result.message;
    const auto after = editor.project();
    const auto check = [&](const EditorViewModel& current) {
        const auto& clips = current.activeTimeline()->tracks[0].clips;
        ASSERT_EQ(clips.size(), 2u);
        EXPECT_EQ(clips[0].trimEndFrame, original.trimEndFrame + 30);
        EXPECT_EQ(clips[1].trimStartFrame, original.trimStartFrame + 18);
        expectSamples(clips[0], original);
        expectSamples(clips[1], original, 9);
        expectAnchors(clips[0]);
        expectAnchors(clips[1]);
    };
    check(editor);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), before);
    expectSamples(*editor.activeTimeline()->clipById(original.id), original);
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);
    check(editor);
    TemporaryPackage package;
    ASSERT_TRUE(project.savePackage(package.utf8(), &error)) << error;
    VideoProject reopened;
    ASSERT_TRUE(reopened.loadPackage(package.utf8(), &error)) << error;
    EXPECT_EQ(reopened.editor().project(), after);
    check(reopened.editor());
}

TEST_P(AppearanceEditContinuityTest, BothTrimsAndExtensionsRestoreEveryFrameThroughHistoryAndReopen) {
    const auto original = animatedClip(GetParam());
    VideoProject project;
    auto& editor = project.editor();
    std::string error;
    ASSERT_TRUE(install(editor, original, error)) << error;
    int64_t offset = 0;
    // Keep the saved source package available for every subsequent save.
    TemporaryPackage package;
    for (const auto& edit : std::vector<std::pair<TrimEdge, int64_t>>{
             {TrimEdge::Left, 5}, {TrimEdge::Right, -4},
             {TrimEdge::Left, -5}, {TrimEdge::Right, 4}}) {
        SCOPED_TRACE(edit.second);
        const auto before = editor.project();
        const auto result = editor.trimClip(original.id, edit.first, edit.second);
        ASSERT_TRUE(result.changed()) << result.message;
        if (edit.first == TrimEdge::Left) offset += edit.second;
        const auto after = editor.project();
        expectSamples(*editor.activeTimeline()->clipById(original.id), original, offset);
        expectAnchors(*editor.activeTimeline()->clipById(original.id));
        expectHistory(editor, before, after);
        expectSamples(*editor.activeTimeline()->clipById(original.id), original, offset);
        ASSERT_TRUE(project.savePackage(package.utf8(), &error)) << error;
        VideoProject reopened;
        ASSERT_TRUE(reopened.loadPackage(package.utf8(), &error)) << error;
        EXPECT_EQ(reopened.editor().project(), after);
        expectSamples(*reopened.editor().activeTimeline()->clipById(original.id), original, offset);
        expectAnchors(*reopened.editor().activeTimeline()->clipById(original.id));
    }
    EXPECT_EQ(*editor.activeTimeline()->clipById(original.id), original);
}

TEST_P(AppearanceEditContinuityTest, SingleAndGroupMovesKeepClipLocalCurvesThroughHistoryAndReopen) {
    const auto original = animatedClip(GetParam());
    for (const bool group : {false, true}) {
        SCOPED_TRACE(group);
        VideoProject project;
        auto& editor = project.editor();
        std::string error;
        ASSERT_TRUE(install(editor, original, error)) << error;
        const auto before = editor.project();
        const auto result = group
            ? editor.moveClips({{original.id, "other-video", 61}})
            : editor.moveClip(original.id, 61, "other-video");
        ASSERT_TRUE(result.changed()) << result.message;
        const auto after = editor.project();
        auto expected = original;
        expected.startFrame = 61;
        EXPECT_EQ(*editor.activeTimeline()->clipById(original.id), expected);
        expectSamples(*editor.activeTimeline()->clipById(original.id), original);
        expectHistory(editor, before, after);
        expectSamples(*editor.activeTimeline()->clipById(original.id), original);
        TemporaryPackage package;
        ASSERT_TRUE(project.savePackage(package.utf8(), &error)) << error;
        VideoProject reopened;
        ASSERT_TRUE(reopened.loadPackage(package.utf8(), &error)) << error;
        EXPECT_EQ(reopened.editor().project(), after);
        expectSamples(*reopened.editor().activeTimeline()->clipById(original.id), original);
        expectAnchors(*reopened.editor().activeTimeline()->clipById(original.id));
    }
}

TEST_P(AppearanceEditContinuityTest, OverwriteAndInsertPreserveSurvivingFramesAndHistory) {
    const auto original = animatedClip(GetParam());
    for (const bool insert : {false, true}) {
        SCOPED_TRACE(insert);
        VideoProject project;
        auto& editor = project.editor();
        std::string error;
        ASSERT_TRUE(install(editor, original, error)) << error;
        const auto before = editor.project();
        const auto result = insert ? editor.insertClip("media", 19, 4, 0, "video")
                                   : editor.placeClip("media", 19, 4, 0, "video");
        ASSERT_TRUE(result.changed()) << result.message;
        const auto after = editor.project();
        expectHistory(editor, before, after);
        const auto& clips = editor.activeTimeline()->tracks[0].clips;
        ASSERT_EQ(clips.size(), 3u);
        expectSamples(clips[0], original);
        expectSamples(clips[2], original, insert ? 9 : 13);
        expectAnchors(clips[0]);
        expectAnchors(clips[2]);
        TemporaryPackage package;
        ASSERT_TRUE(project.savePackage(package.utf8(), &error)) << error;
        VideoProject reopened;
        ASSERT_TRUE(reopened.loadPackage(package.utf8(), &error)) << error;
        EXPECT_EQ(reopened.editor().project(), after);
        const auto& loaded = reopened.editor().activeTimeline()->tracks[0].clips;
        ASSERT_EQ(loaded.size(), 3u);
        expectSamples(loaded[0], original);
        expectSamples(loaded[2], original, insert ? 9 : 13);
        expectAnchors(loaded[0]);
        expectAnchors(loaded[2]);
    }
}

INSTANTIATE_TEST_SUITE_P(LinearHoldSmooth, AppearanceEditContinuityTest,
                        ::testing::Values(std::string("linear"), std::string("hold"), std::string("smooth")));

void setBoundaryTrack(Clip& clip, size_t field, const JsonValue& frame) {
    JsonValue firstValue = 0.5, lastValue = 0.7;
    if (field < appearanceFields.size()) {
        const auto& keys = (clip.*appearanceFields[field])->value.at("keyframes");
        firstValue = keys.front().at("value");
        lastValue = keys.back().at("value");
    }
    const bool negative = !frame.is_number_unsigned() && frame.get<int64_t>() < 0;
    const auto payload = KeyframeTrackPayload{JsonValue{{"keyframes", JsonValue::array({
        JsonValue{{"frame", negative ? frame : JsonValue(0)}, {"value", firstValue}},
        JsonValue{{"frame", negative ? JsonValue(0) : frame}, {"value", lastValue}}})}}};
    if (field < appearanceFields.size()) clip.*appearanceFields[field] = payload;
    else if (field == appearanceFields.size()) clip.volumeTrack = payload;
    else clip.effects = JsonValue::array({JsonValue{{"type", "video.basicGrade"},
        {"params", JsonValue{{"exposure", JsonValue{{"track", payload.value}}}}}}});
}

TEST(AppearanceEditOverflowTest, SignedUnderflowRejectsEverySplittingAndOverwriteEntryWithoutTouchingHistory) {
    using Edit = std::function<EditResult(EditorViewModel&)>;
    const std::vector<Edit> edits{
        [](auto& e) { return e.splitClip("animated", 15); },
        [](auto& e) { return e.trimClip("animated", TrimEdge::Left, 5); },
        [](auto& e) { return e.placeClip("media", 15, 4, 0, "video"); },
        [](auto& e) { return e.placeClip("media", 5, 10, 0, "video"); },
        [](auto& e) { return e.insertClip("media", 15, 4, 0, "video"); },
        [](auto& e) { return e.moveClip("mover", 15); },
        [](auto& e) { return e.moveClips({{"mover", "video", 15}}); },
        [](auto& e) { return e.trimClip("head", TrimEdge::Right, 7); }};
    for (size_t field = 0; field < appearanceFields.size() + 2; ++field) {
        for (size_t edit = 0; edit < edits.size(); ++edit) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(edit);
            auto original = animatedClip("linear");
            setBoundaryTrack(original, field, JsonValue(std::numeric_limits<int64_t>::min()));
            Clip mover;
            mover.id = "mover";
            mover.mediaRef = "media";
            mover.startFrame = 70;
            mover.durationFrames = 4;
            Clip head = mover;
            head.id = "head";
            head.startFrame = 5;
            head.durationFrames = 3;
            head.trimEndFrame = 100;
            EditorViewModel editor;
            std::string error;
            ASSERT_TRUE(install(editor, original, error, {head, mover})) << error;
            ASSERT_TRUE(editor.setClipOpacity("mover", 0.5).changed());
            const auto redoState = editor.project();
            ASSERT_TRUE(editor.undo());
            const auto before = editor.project();
            const auto revision = editor.revision();
            const auto result = edits[edit](editor);
            EXPECT_EQ(result.status, EditStatus::Invalid) << result.message;
            EXPECT_TRUE(result.affectedIds.empty());
            EXPECT_FALSE(result.message.empty());
            EXPECT_EQ(editor.project(), before);
            EXPECT_EQ(editor.revision(), revision);
            EXPECT_FALSE(editor.undo());
            ASSERT_TRUE(editor.redo());
            EXPECT_EQ(editor.project(), redoState);
        }
    }
}

TEST(AppearanceEditOverflowTest, SecondOverwriteSplitFailureDiscardsTheFirstSuccessfulSplit) {
    auto original = animatedClip("smooth");
    setBoundaryTrack(original, 0, JsonValue(std::numeric_limits<int64_t>::min() + 10));
    EditorViewModel editor;
    std::string error;
    ASSERT_TRUE(install(editor, original, error)) << error;
    const auto before = editor.project();
    const auto revision = editor.revision();
    // The first shift by -9 fits; the following shift by -4 does not.
    EXPECT_EQ(editor.placeClip("media", 19, 4, 0, "video").status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), before);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.undo());
    EXPECT_FALSE(editor.redo());
}

TEST(AppearanceEditOverflowTest, SignedAndRepresentableUnsignedOverflowRejectLeftExtensionAtomically) {
    const auto maximum = std::numeric_limits<int64_t>::max();
    for (const auto& frame : {JsonValue(maximum), JsonValue(static_cast<uint64_t>(maximum))}) {
        for (size_t field = 0; field < appearanceFields.size() + 2; ++field) {
            SCOPED_TRACE(field);
            auto original = animatedClip("linear");
            setBoundaryTrack(original, field, frame);
            EditorViewModel editor;
            std::string error;
            ASSERT_TRUE(install(editor, original, error)) << error;
            ASSERT_TRUE(editor.moveClip("animated", 11).changed());
            const auto redoState = editor.project();
            ASSERT_TRUE(editor.undo());
            const auto before = editor.project();
            const auto revision = editor.revision();
            EXPECT_EQ(editor.trimClip("animated", TrimEdge::Left, -5).status, EditStatus::Invalid);
            EXPECT_EQ(editor.project(), before);
            EXPECT_EQ(editor.revision(), revision);
            EXPECT_FALSE(editor.undo());
            ASSERT_TRUE(editor.redo());
            EXPECT_EQ(editor.project(), redoState);
        }
    }
}

TEST(AppearanceEditOverflowTest, OpaqueAndOutOfSignedRangeUnsignedEntriesSurviveUnrelatedAndTimedEdits) {
    const JsonValue opaque = JsonValue::parse(R"({"opaque":"keep","keyframes":[null,{"future":true},{"frame":"future","value":{}},{"frame":1.5,"value":2},{"frame":9223372036854775808,"value":3},{"frame":18446744073709551615,"value":4},{"frame":3,"value":0.7}]})");
    auto original = animatedClip("linear");
    for (const auto field : appearanceFields) original.*field = KeyframeTrackPayload{opaque};
    original.volumeTrack = KeyframeTrackPayload{opaque};
    original.effects = JsonValue::array({JsonValue{{"type", "future.effect"}, {"payload", opaque}}});
    EditorViewModel editor;
    std::string error;
    ASSERT_TRUE(install(editor, original, error)) << error;
    ASSERT_TRUE(editor.setClipOpacity("animated", 0.4).changed());
    auto expected = original;
    expected.opacity = 0.4;
    EXPECT_EQ(*editor.activeTimeline()->clipById("animated"), expected);
    ASSERT_TRUE(editor.moveClip("animated", 40).changed());
    expected.startFrame = 40;
    EXPECT_EQ(*editor.activeTimeline()->clipById("animated"), expected);
    ASSERT_TRUE(editor.trimClip("animated", TrimEdge::Right, -2).changed());
    expected.durationFrames -= 2;
    expected.trimEndFrame += 4;
    EXPECT_EQ(*editor.activeTimeline()->clipById("animated"), expected);
    const auto before = editor.project();
    ASSERT_TRUE(editor.trimClip("animated", TrimEdge::Left, 5).changed());
    auto shifted = opaque;
    shifted["keyframes"][6]["frame"] = -2;
    const auto& trimmed = *editor.activeTimeline()->clipById("animated");
    for (const auto field : appearanceFields) EXPECT_EQ((trimmed.*field)->value, shifted);
    EXPECT_EQ(trimmed.volumeTrack->value, shifted);
    EXPECT_EQ((*trimmed.effects)[0]["payload"], shifted);
    const auto after = editor.project();
    expectHistory(editor, before, after);
}

TEST(AppearanceEditOverflowTest, UnshiftedExtremeAnchorsDoNotBlockMoveOrRightTrim) {
    auto original = animatedClip("linear");
    setBoundaryTrack(original, 0, JsonValue(std::numeric_limits<int64_t>::min()));
    setBoundaryTrack(original, appearanceFields.size() + 1, JsonValue(std::numeric_limits<int64_t>::max()));
    EditorViewModel editor;
    std::string error;
    ASSERT_TRUE(install(editor, original, error)) << error;
    ASSERT_TRUE(editor.moveClip("animated", 50).changed());
    auto expected = original;
    expected.startFrame = 50;
    EXPECT_EQ(*editor.activeTimeline()->clipById("animated"), expected);
    ASSERT_TRUE(editor.trimClip("animated", TrimEdge::Right, -4).changed());
    expected.durationFrames -= 4;
    expected.trimEndFrame += 8;
    EXPECT_EQ(*editor.activeTimeline()->clipById("animated"), expected);
}
} // namespace
