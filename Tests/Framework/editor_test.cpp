#include <Editor/EditorViewModel.hpp>
#include <Models/MediaResolver.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>

namespace {

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.c_str()), value.size()};
}

std::filesystem::path tempPackage(const std::string& suffix) {
    return std::filesystem::temp_directory_path() /
           std::filesystem::u8path("heisenberg-" + suffix + "-" + heisenberg::makeUuid() + ".palmier");
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

heisenberg::MediaManifest testManifest() {
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry oldMedia;
    oldMedia.id = "media-old";
    oldMedia.name = "Old";
    oldMedia.source = {heisenberg::MediaSource::Kind::External, "C:/offline-old.mp4"};
    oldMedia.duration = 10.0;
    EXPECT_TRUE(manifest.add(oldMedia));
    heisenberg::MediaManifestEntry newMedia = oldMedia;
    newMedia.id = "media-new";
    newMedia.name = "New";
    newMedia.source.path = "C:/offline-new.mp4";
    EXPECT_TRUE(manifest.add(newMedia));
    return manifest;
}

heisenberg::ProjectFile testProject(const heisenberg::Clip& initial, bool secondTrack = false) {
    heisenberg::ProjectFile project;
    heisenberg::Timeline timeline;
    timeline.id = "timeline-main";
    timeline.fps = 30;
    heisenberg::Track track;
    track.id = "track-video";
    track.clips.push_back(initial);
    timeline.tracks.push_back(track);
    if (secondTrack) {
        heisenberg::Track another;
        another.id = "track-audio";
        another.type = heisenberg::ClipType::Audio;
        timeline.tracks.push_back(another);
    }
    project.timelines.push_back(timeline);
    project.activeTimelineId = timeline.id;
    project.openTimelineIds = std::vector<std::string>{timeline.id};
    return project;
}

heisenberg::Clip initialClip(int64_t start = 0, int64_t duration = 20) {
    heisenberg::Clip clip;
    clip.id = "clip-old";
    clip.mediaRef = "media-old";
    clip.startFrame = start;
    clip.durationFrames = duration;
    return clip;
}

std::unique_ptr<heisenberg::EditorViewModel> loadEditor(
    const std::filesystem::path& root, const heisenberg::ProjectFile& project,
    const heisenberg::MediaManifest& manifest) {
    writeText(root / "project.json", heisenberg::ProjectCodec::encode(project));
    writeText(root / "media.json", manifest.toJson());
    std::filesystem::create_directories(root / "media");
    auto editor = std::make_unique<heisenberg::EditorViewModel>();
    std::string error;
    EXPECT_TRUE(editor->loadPackage(utf8(root), &error)) << error;
    return editor;
}

const heisenberg::Clip* byMedia(const heisenberg::Track& track, const std::string& mediaRef) {
    for (const auto& clip : track.clips) if (clip.mediaRef == mediaRef) return &clip;
    return nullptr;
}

} // namespace

TEST(MediaResolverTest, ResolvesExternalAndProjectSources) {
    const auto root = tempPackage("resolver");
    writeText(root / "media" / "inside.mp4", "x");
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry external;
    external.id = "external";
    external.source = {heisenberg::MediaSource::Kind::External, HEISENBERG_PLAYLIST_CLIP_A};
    external.duration = 1.0;
    ASSERT_TRUE(manifest.add(external));
    heisenberg::MediaManifestEntry project;
    project.id = "project";
    project.source = {heisenberg::MediaSource::Kind::Project, "media/inside.mp4"};
    ASSERT_TRUE(manifest.add(project));
    heisenberg::MediaResolver resolver(manifest, utf8(root));
    EXPECT_FALSE(resolver.isMissing("external"));
    EXPECT_FALSE(resolver.isMissing("project"));
    EXPECT_TRUE(resolver.isMissing("unknown"));
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, ImportUsesStableUuidMediaIds) {
    heisenberg::EditorViewModel editor;
    editor.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(editor.importMedia(HEISENBERG_PLAYLIST_CLIP_A, &error)) << error;
    ASSERT_EQ(editor.manifest().entries().size(), 1u);
    const std::string& id = editor.manifest().entries().front().id;
    EXPECT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_NE(id, "media_1");
}

TEST(EditorViewModelTest, OverwriteHandlesAllFourIntersectionShapes) {
    const auto manifest = testManifest();
    std::vector<std::filesystem::path> roots;

    auto make = [&](const std::string& name, int64_t start, int64_t duration) {
        const auto root = tempPackage(name);
        roots.push_back(root);
        return loadEditor(root, testProject(initialClip(start, duration)), manifest);
    };

    {
        auto editor = make("overwrite-remove", 10, 10);
        ASSERT_TRUE(editor->placeClip("media-new", 5, 20).changed());
        ASSERT_EQ(editor->activeTrack()->clips.size(), 1u);
        EXPECT_EQ(editor->activeTrack()->clips.front().mediaRef, "media-new");
    }
    {
        auto editor = make("overwrite-left", 0, 10);
        ASSERT_TRUE(editor->placeClip("media-new", 5, 10).changed());
        const auto* old = editor->activeTrack()->clipById("clip-old");
        ASSERT_NE(old, nullptr);
        EXPECT_EQ(old->startFrame, 0);
        EXPECT_EQ(old->durationFrames, 5);
        EXPECT_EQ(old->trimEndFrame, 5);
    }
    {
        auto editor = make("overwrite-right", 10, 10);
        ASSERT_TRUE(editor->placeClip("media-new", 5, 10).changed());
        const auto* old = editor->activeTrack()->clipById("clip-old");
        ASSERT_NE(old, nullptr);
        EXPECT_EQ(old->startFrame, 15);
        EXPECT_EQ(old->durationFrames, 5);
        EXPECT_EQ(old->trimStartFrame, 5);
    }
    {
        auto editor = make("overwrite-split", 0, 20);
        ASSERT_TRUE(editor->placeClip("media-new", 5, 10).changed());
        ASSERT_EQ(editor->activeTrack()->clips.size(), 3u);
        EXPECT_EQ(editor->activeTrack()->clips[0].startFrame, 0);
        EXPECT_EQ(editor->activeTrack()->clips[0].durationFrames, 5);
        EXPECT_EQ(editor->activeTrack()->clips[1].mediaRef, "media-new");
        EXPECT_EQ(editor->activeTrack()->clips[2].startFrame, 15);
        EXPECT_EQ(editor->activeTrack()->clips[2].trimStartFrame, 15);
    }

    std::error_code code;
    for (const auto& root : roots) std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, MoveDoesNotOverwriteItself) {
    const auto root = tempPackage("move-self");
    auto editor = loadEditor(root, testProject(initialClip(0, 10)), testManifest());
    ASSERT_TRUE(editor->moveClip("clip-old", 2).changed());
    ASSERT_EQ(editor->activeTrack()->clips.size(), 1u);
    EXPECT_EQ(editor->activeTrack()->clips.front().id, "clip-old");
    EXPECT_EQ(editor->activeTrack()->clips.front().startFrame, 2);
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, SplitTrimAndUndoPreserveTimingPayloads) {
    heisenberg::Clip clip = initialClip(0, 10);
    clip.speed = 2.0;
    clip.fadeInFrames = 3;
    clip.fadeOutFrames = 4;
    clip.opacityTrack = heisenberg::KeyframeTrackPayload{nlohmann::ordered_json::parse(
        R"json({"keyframes":[{"frame":2,"value":0.2},{"frame":6,"value":0.8}]})json")};
    clip.effects = nlohmann::ordered_json::parse(
        R"json([{"id":"blur","type":"video.blur","enabled":true,"params":{"radius":{"value":2.0,"track":{"keyframes":[{"frame":2,"value":3.0},{"frame":6,"value":5.0}]}}}}])json");
    clip.wordTimings = std::vector<heisenberg::WordTiming>{{"one", 1, 3}, {"two", 5, 8}};
    const auto root = tempPackage("split-trim");
    auto editor = loadEditor(root, testProject(clip), testManifest());
    const auto before = nlohmann::ordered_json::parse(heisenberg::ProjectCodec::encode(editor->project()));

    const auto split = editor->splitClip("clip-old", 4);
    ASSERT_TRUE(split.changed());
    ASSERT_EQ(split.affectedIds.size(), 2u);
    const auto* left = editor->activeTrack()->clipById("clip-old");
    const auto* right = editor->activeTrack()->clipById(split.affectedIds[1]);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->durationFrames, 4);
    EXPECT_EQ(left->trimEndFrame, 12);
    EXPECT_EQ(left->fadeOutFrames, 0);
    EXPECT_EQ(right->startFrame, 4);
    EXPECT_EQ(right->trimStartFrame, 8);
    EXPECT_EQ(right->fadeInFrames, 0);
    ASSERT_TRUE(right->opacityTrack.has_value());
    EXPECT_EQ(right->opacityTrack->value["keyframes"][0]["frame"], 2);
    EXPECT_EQ((*right->effects)[0]["params"]["radius"]["track"]["keyframes"][0]["frame"], 2);
    const auto afterSplit = nlohmann::ordered_json::parse(heisenberg::ProjectCodec::encode(editor->project()));
    ASSERT_TRUE(editor->undo());
    EXPECT_EQ(nlohmann::ordered_json::parse(heisenberg::ProjectCodec::encode(editor->project())), before);
    ASSERT_TRUE(editor->redo());
    EXPECT_EQ(nlohmann::ordered_json::parse(heisenberg::ProjectCodec::encode(editor->project())), afterSplit);

    ASSERT_TRUE(editor->undo());
    const auto trim = editor->trimClip("clip-old", heisenberg::TrimEdge::Left, 2);
    ASSERT_TRUE(trim.changed());
    const auto* trimmed = editor->activeTrack()->clipById("clip-old");
    ASSERT_NE(trimmed, nullptr);
    EXPECT_EQ(trimmed->startFrame, 2);
    EXPECT_EQ(trimmed->durationFrames, 8);
    EXPECT_EQ(trimmed->trimStartFrame, 4);
    ASSERT_TRUE(trimmed->opacityTrack.has_value());
    EXPECT_EQ(trimmed->opacityTrack->value["keyframes"][0]["frame"], 0);
    EXPECT_EQ(trimmed->opacityTrack->value["keyframes"][1]["frame"], 4);
    EXPECT_EQ((*trimmed->effects)[0]["params"]["radius"]["track"]["keyframes"][0]["frame"], 0);
    EXPECT_EQ((*trimmed->effects)[0]["params"]["radius"]["track"]["keyframes"][1]["frame"], 4);

    const auto beforeFailure = heisenberg::ProjectCodec::encode(editor->project());
    EXPECT_EQ(editor->trimClip("clip-old", heisenberg::TrimEdge::Left, 99).status,
              heisenberg::EditStatus::Invalid);
    EXPECT_EQ(heisenberg::ProjectCodec::encode(editor->project()), beforeFailure);
    ASSERT_TRUE(editor->undo());
    EXPECT_EQ(nlohmann::ordered_json::parse(heisenberg::ProjectCodec::encode(editor->project())), before);
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, SplitBoundaryPayloadBelongsToRightAndRightTrimCropsTiming) {
    auto clip = initialClip(30, 10);
    clip.speed = 2.0;
    clip.trimStartFrame = 2;
    clip.trimEndFrame = 4;
    clip.fadeInFrames = 3;
    clip.fadeOutFrames = 4;
    clip.opacityTrack = heisenberg::KeyframeTrackPayload{nlohmann::ordered_json::parse(
        R"json({"keyframes":[{"frame":2,"value":0.2},{"frame":4,"value":0.4},{"frame":6,"value":0.8}]})json")};
    clip.effects = nlohmann::ordered_json::parse(
        R"json([{"id":"blur","type":"video.blur","enabled":true,"params":{"radius":{"value":2.0,"track":{"keyframes":[{"frame":4,"value":3.0},{"frame":6,"value":5.0}]}}}}])json");
    clip.wordTimings = std::vector<heisenberg::WordTiming>{{"cross", 2, 6}, {"last", 7, 9}};
    const auto root = tempPackage("split-boundary");
    auto editor = loadEditor(root, testProject(clip), testManifest());
    const auto before = heisenberg::ProjectCodec::encode(editor->project());
    const auto split = editor->splitClip(clip.id, 34);
    ASSERT_TRUE(split.changed());
    const auto* left = editor->activeTrack()->clipById(clip.id);
    const auto* right = editor->activeTrack()->clipById(split.affectedIds[1]);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->trimStartFrame, 2);
    EXPECT_EQ(left->trimEndFrame, 16);
    EXPECT_EQ(left->fadeInFrames, 3);
    EXPECT_EQ(left->fadeOutFrames, 0);
    EXPECT_EQ(right->trimStartFrame, 10);
    EXPECT_EQ(right->trimEndFrame, 4);
    EXPECT_EQ(right->fadeInFrames, 0);
    EXPECT_EQ(right->fadeOutFrames, 4);
    ASSERT_TRUE(left->opacityTrack);
    EXPECT_EQ(left->opacityTrack->value["keyframes"].size(), 1u);
    ASSERT_TRUE(right->opacityTrack);
    EXPECT_EQ(right->opacityTrack->value["keyframes"][0]["frame"], 0);
    EXPECT_EQ(right->opacityTrack->value["keyframes"][1]["frame"], 2);
    EXPECT_TRUE((*left->effects)[0]["params"]["radius"]["track"]["keyframes"].empty());
    EXPECT_EQ((*right->effects)[0]["params"]["radius"]["track"]["keyframes"][0]["frame"], 0);
    ASSERT_TRUE(left->wordTimings);
    ASSERT_TRUE(right->wordTimings);
    EXPECT_EQ(*left->wordTimings, (std::vector<heisenberg::WordTiming>{{"cross", 2, 4}}));
    EXPECT_EQ(*right->wordTimings, (std::vector<heisenberg::WordTiming>{{"cross", 0, 2}, {"last", 3, 5}}));
    ASSERT_TRUE(editor->undo());
    EXPECT_EQ(heisenberg::ProjectCodec::encode(editor->project()), before);

    ASSERT_TRUE(editor->trimClip(clip.id, heisenberg::TrimEdge::Right, -5).changed());
    const auto* trimmed = editor->activeTrack()->clipById(clip.id);
    EXPECT_EQ(trimmed->durationFrames, 5);
    EXPECT_EQ(trimmed->trimEndFrame, 14);
    EXPECT_EQ(trimmed->fadeInFrames, 3);
    EXPECT_EQ(trimmed->fadeOutFrames, 2);
    EXPECT_EQ(trimmed->opacityTrack->value["keyframes"].size(), 2u);
    EXPECT_EQ((*trimmed->effects)[0]["params"]["radius"]["track"]["keyframes"].size(), 1u);
    EXPECT_EQ(*trimmed->wordTimings, (std::vector<heisenberg::WordTiming>{{"cross", 2, 5}}));
    ASSERT_TRUE(editor->undo());
    EXPECT_EQ(heisenberg::ProjectCodec::encode(editor->project()), before);
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, ExtremeTrimDeltasAreRejectedWithoutHistory) {
    const auto root = tempPackage("trim-overflow");
    auto editor = loadEditor(root, testProject(initialClip()), testManifest());
    const auto before = heisenberg::ProjectCodec::encode(editor->project());
    for (const auto edge : {heisenberg::TrimEdge::Left, heisenberg::TrimEdge::Right}) {
        for (const auto delta : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
            EXPECT_EQ(editor->trimClip("clip-old", edge, delta).status, heisenberg::EditStatus::Invalid);
            EXPECT_EQ(heisenberg::ProjectCodec::encode(editor->project()), before);
            EXPECT_FALSE(editor->undo());
        }
    }
    EXPECT_EQ(editor->splitClip("clip-old", std::numeric_limits<int64_t>::max()).status, heisenberg::EditStatus::NoOp);
    EXPECT_FALSE(editor->undo());
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, MultiTrackTimelineIsPreservedButUnsupportedForEditing) {
    const auto root = tempPackage("multi-track");
    const auto project = testProject(initialClip(), true);
    auto editor = loadEditor(root, project, testManifest());
    const auto before = heisenberg::ProjectCodec::encode(editor->project());
    EXPECT_EQ(editor->compatibility().level, heisenberg::CompatibilityLevel::Unsupported);
    EXPECT_EQ(editor->placeClip("media-new", 30, 5).status, heisenberg::EditStatus::Unsupported);
    EXPECT_EQ(heisenberg::ProjectCodec::encode(editor->project()), before);
    std::error_code code;
    std::filesystem::remove_all(root, code);
}

TEST(EditorViewModelTest, PackageSaveAsPreservesUnknownFiles) {
    const auto source = tempPackage("save-source");
    const auto destination = tempPackage("save-copy");
    heisenberg::EditorViewModel editor;
    std::string error;
    ASSERT_TRUE(editor.savePackage(utf8(source), &error)) << error;
    writeText(source / "thumbnail.png", "thumbnail-bytes");
    writeText(source / "chat" / "history.json", R"json({"messages":[]})json");
    writeText(source / "custom" / "unknown.bin", "unknown");
    writeText(source / "media" / "keep.dat", "media");

    ASSERT_TRUE(editor.savePackage(utf8(destination), &error)) << error;
    EXPECT_TRUE(std::filesystem::exists(destination / "project.json"));
    EXPECT_TRUE(std::filesystem::exists(destination / "media.json"));
    EXPECT_TRUE(std::filesystem::exists(destination / "thumbnail.png"));
    EXPECT_TRUE(std::filesystem::exists(destination / "chat" / "history.json"));
    EXPECT_TRUE(std::filesystem::exists(destination / "custom" / "unknown.bin"));
    EXPECT_TRUE(std::filesystem::exists(destination / "media" / "keep.dat"));

    heisenberg::EditorViewModel loaded;
    ASSERT_TRUE(loaded.loadPackage(utf8(destination), &error)) << error;
    EXPECT_EQ(loaded.project().timelines.size(), 1u);
    std::error_code code;
    std::filesystem::remove_all(source, code);
    std::filesystem::remove_all(destination, code);
}

TEST(EditorViewModelTest, MissingMediaAndNoOpsDoNotCreateHistory) {
    heisenberg::EditorViewModel editor;
    EXPECT_EQ(editor.placeClip("missing", 0, 4).status, heisenberg::EditStatus::Invalid);
    EXPECT_EQ(editor.removeClips({"missing"}).status, heisenberg::EditStatus::Unsupported);
    EXPECT_FALSE(editor.undo());
}
