#include <Editor/EditorViewModel.hpp>
#include <Models/MediaManifest.hpp>
#include <Models/MediaResolver.hpp>
#include <Models/Timeline.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string utf8FromPath(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

} // namespace

TEST(MediaManifestTest, LoadsEntriesAndRejectsDuplicateIds) {
    const std::string json =
        "{\n"
        "  \"version\": 1,\n"
        "  \"entries\": [\n"
        "    {\n"
        "      \"id\": \"clip_a\",\n"
        "      \"name\": \"A\",\n"
        "      \"type\": \"video\",\n"
        "      \"source\": { \"kind\": \"external\", \"path\": \"C:/a.mp4\" },\n"
        "      \"durationFrames\": 24\n"
        "    }\n"
        "  ]\n"
        "}\n";

    heisenberg::MediaManifest manifest;
    std::string error;
    ASSERT_TRUE(manifest.loadFromJson(json, &error)) << error;
    ASSERT_EQ(manifest.entries().size(), 1u);
    ASSERT_NE(manifest.find("clip_a"), nullptr);
    EXPECT_EQ(manifest.find("clip_a")->durationFrames, 24);

    heisenberg::MediaManifestEntry duplicate;
    duplicate.id = "clip_a";
    duplicate.source.path = "C:/b.mp4";
    EXPECT_FALSE(manifest.add(duplicate, &error));
}

TEST(MediaResolverTest, ResolvesExternalAndProjectPaths) {
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry external;
    external.id = "ext";
    external.name = "External";
    external.source.kind = heisenberg::MediaSource::Kind::External;
    external.source.path = HEISENBERG_PLAYLIST_CLIP_A;
    external.durationFrames = 24;
    ASSERT_TRUE(manifest.add(external));

    heisenberg::MediaManifestEntry missing;
    missing.id = "gone";
    missing.name = "Gone";
    missing.source.kind = heisenberg::MediaSource::Kind::External;
    missing.source.path = "C:/definitely-missing-heisenberg.mp4";
    ASSERT_TRUE(manifest.add(missing));

    heisenberg::MediaResolver resolver(manifest);
    EXPECT_EQ(resolver.resolvePath("ext"), HEISENBERG_PLAYLIST_CLIP_A);
    EXPECT_FALSE(resolver.isMissing("ext"));
    EXPECT_TRUE(resolver.resolvePath("gone").empty());
    EXPECT_TRUE(resolver.isMissing("gone"));
    EXPECT_EQ(resolver.displayName("gone"), "Gone");
    EXPECT_EQ(resolver.displayName("unknown"), "Offline");
}

TEST(EditorViewModelTest, ImportPlaceMoveRemoveAndRoundTripProject) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    const std::string clipB = HEISENBERG_PLAYLIST_CLIP_B;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipB)));

    heisenberg::EditorViewModel editor;
    editor.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(editor.importMedia(clipA, &error)) << error;
    ASSERT_TRUE(editor.importMedia(clipB, &error)) << error;
    ASSERT_EQ(editor.assets().size(), 2u);
    EXPECT_FALSE(editor.assets()[0].missing);
    EXPECT_EQ(editor.assets()[0].id, "media_1");
    EXPECT_EQ(editor.assets()[1].id, "media_2");

    const std::string first = editor.placeClip("media_1", 0, 0, 9, &error);
    ASSERT_FALSE(first.empty()) << error;
    const std::string second = editor.placeClip("media_2", 24, 0, 4, &error);
    ASSERT_FALSE(second.empty()) << error;
    ASSERT_EQ(editor.timeline().clips().size(), 2u);
    EXPECT_EQ(editor.timeline().clips()[0].mediaRef, "media_1");
    EXPECT_EQ(editor.timeline().clips()[1].start, 24);
    ASSERT_EQ(editor.timeline().length(), 29);

    ASSERT_TRUE(editor.moveClip(second, 10, &error)) << error;
    EXPECT_EQ(editor.timeline().clipById(second)->start, 10);
    ASSERT_EQ(editor.timeline().length(), 15);

    const auto temp = std::filesystem::temp_directory_path() / "heisenberg_editor_test";
    std::error_code code;
    std::filesystem::remove_all(temp, code);
    ASSERT_TRUE(editor.save(utf8FromPath(temp), &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(temp / "media.json"));
    ASSERT_TRUE(std::filesystem::exists(temp / "timeline.json"));

    heisenberg::EditorViewModel loaded;
    loaded.setHardwareDecode(false);
    ASSERT_TRUE(loaded.load(utf8FromPath(temp), &error)) << error;
    ASSERT_EQ(loaded.assets().size(), 2u);
    ASSERT_EQ(loaded.timeline().clips().size(), 2u);
    EXPECT_EQ(loaded.timeline().clipById(second)->start, 10);
    EXPECT_EQ(loaded.timeline().clips()[0].mediaRef, "media_1");
    EXPECT_FALSE(loaded.resolver().isMissing("media_1"));

    ASSERT_TRUE(loaded.removeClip(second, &error)) << error;
    ASSERT_EQ(loaded.timeline().clips().size(), 1u);
    EXPECT_EQ(loaded.timeline().clips()[0].id, first);
    std::filesystem::remove_all(temp, code);
}

TEST(EditorViewModelTest, PlaceClipRejectsMissingMedia) {
    heisenberg::EditorViewModel editor;
    std::string error;
    EXPECT_TRUE(editor.placeClip("missing", 0, 0, 4, &error).empty());
    EXPECT_NE(error.find("missing"), std::string::npos);
}
