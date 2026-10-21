#include <Editor/EditorViewModel.hpp>
#include <Compositing/CompositionBuilder.hpp>
#include <Models/ProjectCodec.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

using namespace heisenberg;

namespace {
MediaManifest independentManifest() {
    MediaManifest manifest;
    MediaManifestEntry video;
    video.id = "video-media";
    video.type = ClipType::Video;
    video.source.path = HEISENBERG_PLAYLIST_CLIP_A;
    video.duration = 1.0;
    video.hasAudio = true;
    EXPECT_TRUE(manifest.add(video));
    MediaManifestEntry audio;
    audio.id = "audio-media";
    audio.type = ClipType::Audio;
    audio.source.path = HEISENBERG_TEST_LONG_AUDIO;
    audio.duration = 3.0;
    audio.hasAudio = true;
    EXPECT_TRUE(manifest.add(audio));
    return manifest;
}

ProjectFile independentProject() {
    ProjectFile project;
    Timeline timeline;
    timeline.id = "timeline";
    timeline.fps = 24;
    Track video;
    video.id = "video";
    Clip clip;
    clip.id = "video-clip";
    clip.mediaRef = "video-media";
    clip.durationFrames = 24;
    video.clips.push_back(clip);
    timeline.tracks.push_back(video);
    project.timelines.push_back(timeline);
    project.activeTimelineId = timeline.id;
    return project;
}

void install(EditorViewModel& editor) {
    std::string error;
    ASSERT_TRUE(editor.replaceDocument(independentProject(), independentManifest(), "", &error)) << error;
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct PackageCleanup {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("heisenberg-independent-audio-" + makeUuid() + ".berg");
    ~PackageCleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}

TEST(IndependentAudioEditorTest, ImportsMp3WavAndCoverArtAsAudioOnly) {
    for (const char* path : {HEISENBERG_TEST_MP3, HEISENBERG_TEST_WAV, HEISENBERG_TEST_COVER_MP3}) {
        SCOPED_TRACE(path);
        EditorViewModel editor;
        editor.setHardwareDecode(false);
        std::string error;
        ASSERT_TRUE(editor.importMedia(path, &error)) << error;
        ASSERT_EQ(editor.manifest().entries().size(), 1u);
        const auto entry = editor.manifest().entries().front();
        EXPECT_EQ(entry.type, ClipType::Audio);
        EXPECT_GT(entry.duration, 0.0);
        EXPECT_TRUE(entry.hasAudio.value_or(false));
        EXPECT_FALSE(entry.sourceWidth);
        EXPECT_FALSE(entry.sourceHeight);
        EXPECT_FALSE(entry.sourceFPS);
        const auto placed = editor.placeClip(entry.id, 0);
        ASSERT_TRUE(placed.changed()) << placed.message;
        ASSERT_EQ(editor.activeTimeline()->tracks.size(), 1u);
        const auto& track = editor.activeTimeline()->tracks.front();
        EXPECT_EQ(track.type, ClipType::Audio);
        EXPECT_EQ(editor.activeTrack()->id, track.id);
        EXPECT_EQ(track.clips.front().mediaType, ClipType::Audio);
        EXPECT_EQ(track.clips.front().sourceClipType, ClipType::Audio);
        EXPECT_EQ(editor.compatibility().level, CompatibilityLevel::Supported);
        CompositionPlan plan;
        ASSERT_TRUE(CompositionBuilder::build(*editor.activeTimeline(), editor.manifest(), "", plan, &error)) << error;
        EXPECT_TRUE(plan.clips.empty());
        EXPECT_EQ(plan.audioClips.size(), 1u);
        EXPECT_FALSE(plan.sampleAt(0));
        EXPECT_TRUE(plan.audioSampleAt(0));
    }
}

TEST(IndependentAudioEditorTest, VideoImportKeepsVideoIdentityAndEmbeddedAudio) {
    EditorViewModel editor;
    std::string error;
    ASSERT_TRUE(editor.importMedia(HEISENBERG_PLAYLIST_CLIP_A, &error)) << error;
    const auto& entry = editor.manifest().entries().front();
    EXPECT_EQ(entry.type, ClipType::Video);
    EXPECT_TRUE(entry.hasAudio.value_or(false));
    EXPECT_TRUE(entry.sourceWidth);
    EXPECT_TRUE(entry.sourceHeight);
    EXPECT_TRUE(entry.sourceFPS);
    ASSERT_TRUE(editor.placeClip(entry.id, 0).changed());
    ASSERT_EQ(editor.activeTimeline()->tracks.size(), 1u);
    EXPECT_EQ(editor.activeTimeline()->tracks.front().type, ClipType::Video);
}

TEST(IndependentAudioEditorTest, PlacementTargetsTypeAndRejectsCrossTypeWithoutHistory) {
    EditorViewModel editor;
    install(editor);
    const auto original = editor.project();
    EXPECT_EQ(editor.placeClip("audio-media", 0, 24, 0, "video").status, EditStatus::Invalid);
    EXPECT_EQ(editor.placeClip("video-media", 0, 24, 0, "missing").status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), original);
    EXPECT_FALSE(editor.undo());
    const Track originalVideo = editor.activeTimeline()->tracks.front();
    const auto placed = editor.placeClip("audio-media", 0, 48);
    ASSERT_TRUE(placed.changed());
    ASSERT_EQ(editor.activeTimeline()->tracks.size(), 2u);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), originalVideo);
    const std::string audioId = editor.activeTimeline()->trackForClip(placed.affectedIds.back())->id;
    EXPECT_EQ(editor.activeTimeline()->trackById(audioId)->type, ClipType::Audio);
    EXPECT_EQ(editor.activeTrack()->id, "video"); // Legacy activeTrack is video-first.
    EXPECT_EQ(editor.compatibility().level, CompatibilityLevel::Supported);
    const auto after = editor.project();
    EXPECT_EQ(editor.placeClip("video-media", 0, 24, 0, audioId).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), after);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), original);
    EXPECT_FALSE(editor.undo()); // Automatic track creation and placement were one edit.
    ASSERT_TRUE(editor.redo());
    EXPECT_EQ(editor.project(), after);

    const auto overwrite = editor.placeClip("audio-media", 12, 12, 0, audioId);
    ASSERT_TRUE(overwrite.changed());
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), originalVideo);
    EXPECT_EQ(editor.activeTimeline()->trackById(audioId)->clips.size(), 3u);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), after);
}

TEST(IndependentAudioEditorTest, AudioMoveTrimSplitRemoveTouchOnlyTheirOwnTrackAndUndoOnce) {
    EditorViewModel editor;
    install(editor);
    ASSERT_TRUE(editor.placeClip("audio-media", 0, 48).changed());
    const auto placed = editor.project();
    const Track video = *editor.activeTimeline()->trackById("video");
    const std::string clipId = editor.activeTimeline()->tracks.back().clips.front().id;
    const std::string audioId = editor.activeTimeline()->tracks.back().id;

    ASSERT_TRUE(editor.moveClip(clipId, 4).changed());
    EXPECT_EQ(editor.activeTimeline()->clipById(clipId)->startFrame, 4);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), video);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);

    ASSERT_TRUE(editor.trimClip(clipId, TrimEdge::Left, 4).changed());
    EXPECT_EQ(editor.activeTimeline()->clipById(clipId)->trimStartFrame, 4);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), video);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);

    ASSERT_TRUE(editor.trimClip(clipId, TrimEdge::Right, -4).changed());
    EXPECT_EQ(editor.activeTimeline()->clipById(clipId)->durationFrames, 44);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), video);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);

    const auto split = editor.splitClip(clipId, 24);
    ASSERT_TRUE(split.changed());
    ASSERT_EQ(split.affectedIds.size(), 2u);
    EXPECT_EQ(editor.activeTimeline()->trackForClip(split.affectedIds.back())->id, audioId);
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), video);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);
    ASSERT_TRUE(editor.redo());
    const auto splitDocument = editor.project();
    ASSERT_TRUE(editor.removeClips(split.affectedIds).changed());
    EXPECT_TRUE(editor.activeTimeline()->trackById(audioId)->clips.empty());
    EXPECT_EQ(*editor.activeTimeline()->trackById("video"), video);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), splitDocument); // Multi-remove is one history entry.
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);

    ASSERT_TRUE(editor.moveClip("video-clip", 2).changed());
    EXPECT_EQ(*editor.activeTimeline()->trackById(audioId), placed.timelines.front().tracks.back());
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), placed);
}

TEST(IndependentAudioEditorTest, GainMuteFadesPersistThroughSaveReopenAndBuildTwoContributions) {
    PackageCleanup package;
    VideoProject project;
    install(project.editor());
    auto& editor = project.editor();
    const auto placed = editor.placeClip("audio-media", 0, 72);
    ASSERT_TRUE(placed.changed());
    const std::string clipId = placed.affectedIds.back();
    const std::string audioId = editor.activeTimeline()->trackForClip(clipId)->id;
    const auto beforeAudio = editor.project();
    ASSERT_TRUE(editor.setClipAudio(clipId, 1.5, 12, 18).changed());
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), beforeAudio);
    ASSERT_TRUE(editor.redo());
    const auto beforeMute = editor.project();
    ASSERT_TRUE(editor.setTrackMuted(audioId, true).changed());
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), beforeMute);
    ASSERT_TRUE(editor.redo());
    ASSERT_TRUE(editor.setClipAudio("video-clip", 0.5, 3, 4).changed());
    std::string error;
    ASSERT_TRUE(project.savePackage(utf8(package.path), &error)) << error;
    VideoProject reopened;
    ASSERT_TRUE(reopened.loadPackage(utf8(package.path), &error)) << error;
    EXPECT_EQ(reopened.editor().project(), editor.project());
    EXPECT_EQ(reopened.editor().manifest(), editor.manifest());
    EXPECT_FALSE(reopened.editor().undo());
    const auto* clip = reopened.editor().activeTimeline()->clipById(clipId);
    ASSERT_NE(clip, nullptr);
    EXPECT_DOUBLE_EQ(clip->volume, 1.5);
    EXPECT_EQ(clip->fadeInFrames, 12);
    EXPECT_EQ(clip->fadeOutFrames, 18);
    EXPECT_EQ(clip->fadeInInterpolation, Interpolation::Linear);
    EXPECT_EQ(clip->fadeOutInterpolation, Interpolation::Linear);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(*reopened.editor().activeTimeline(), reopened.editor().manifest(),
        reopened.editor().projectDir(), plan, &error)) << error;
    EXPECT_EQ(plan.frameCount, 72);
    EXPECT_EQ(plan.clips.size(), 1u);
    EXPECT_EQ(plan.audioClips.size(), 1u);
    ASSERT_TRUE(plan.videoSampleAt(6));
    ASSERT_TRUE(plan.audioSampleAt(6));
    EXPECT_DOUBLE_EQ(plan.videoSampleAt(6)->gain, 0.5);
    EXPECT_DOUBLE_EQ(plan.audioSampleAt(6)->gain, 1.5);
    EXPECT_FALSE(plan.videoSampleAt(6)->muted);
    EXPECT_TRUE(plan.audioSampleAt(6)->muted);
    EXPECT_FALSE(plan.videoSampleAt(48));
    EXPECT_TRUE(plan.audioSampleAt(48));
}

TEST(IndependentAudioEditorTest, AudioValidationNoOpsAndNonlinearFieldsArePreserved) {
    EditorViewModel editor;
    install(editor);
    const auto placed = editor.placeClip("audio-media", 0, 48);
    ASSERT_TRUE(placed.changed());
    const std::string id = placed.affectedIds.back();
    auto imported = editor.project();
    auto* importedClip = imported.timelines.front().clipById(id);
    importedClip->fadeInFrames = 8;
    importedClip->fadeOutFrames = 8;
    importedClip->fadeInInterpolation = Interpolation::Smooth;
    importedClip->fadeOutInterpolation = Interpolation::Hold;
    importedClip->volumeTrack = KeyframeTrackPayload{JsonValue::parse(
        R"json({"keyframes":[{"frame":1,"value":0.2,"interpolationOut":"smooth"}]})json")};
    ASSERT_TRUE(editor.replaceDocument(imported, independentManifest()));
    const auto payload = importedClip->volumeTrack;
    const auto revision = editor.revision();
    for (double gain : {-0.1, 2.1, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()})
        EXPECT_EQ(editor.setClipAudio(id, gain, 8, 8).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipAudio(id, 1.0, -1, 8).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipAudio(id, 1.0, 40, 9).status, EditStatus::Invalid);
    EXPECT_EQ(editor.setClipAudio(id, 1.0, 8, 8).status, EditStatus::NoOp);
    EXPECT_EQ(editor.setTrackMuted("missing", true).status, EditStatus::Invalid);
    EXPECT_EQ(editor.project(), imported);
    EXPECT_EQ(editor.revision(), revision);
    EXPECT_FALSE(editor.undo());
    const auto gainOnly = editor.setClipAudio(id, 0.0, 8, 8);
    ASSERT_TRUE(gainOnly.changed());
    EXPECT_FALSE(gainOnly.message.empty());
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->fadeInInterpolation, Interpolation::Smooth);
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->fadeOutInterpolation, Interpolation::Hold);
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->volumeTrack, payload);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), imported);
    const auto editFade = editor.setClipAudio(id, 2.0, 9, 8);
    ASSERT_TRUE(editFade.changed());
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->fadeInInterpolation, Interpolation::Linear);
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->fadeOutInterpolation, Interpolation::Hold);
    EXPECT_EQ(editor.activeTimeline()->clipById(id)->volumeTrack, payload);
    EXPECT_EQ(editor.compatibility().level, CompatibilityLevel::Warning);
    ASSERT_TRUE(editor.undo());
    EXPECT_EQ(editor.project(), imported);
    EXPECT_FALSE(editor.undo());
}
