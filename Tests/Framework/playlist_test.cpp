#include <Producer/Playlist.hpp>
#include <Utiles/Logger.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

bool isCanvas(const heisenberg::ProducerFrame& frame) {
    return frame.hasVideo() &&
           frame.video->width == 1920 &&
           frame.video->height == 1080 &&
           frame.video->format == AV_PIX_FMT_RGBAF16;
}

} // namespace

TEST(PlaylistTest, SequentialGetFrameCrossesCutsOnProfileCanvas) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    const std::string clipB = HEISENBERG_PLAYLIST_CLIP_B;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipB)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a1\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"in\": 0, \"out\": 9 },\n" +
        "    { \"id\": \"b1\", \"resource\": \"" + jsonEscape(clipB) +
        "\", \"in\": 0, \"out\": 4 },\n" +
        "    { \"id\": \"a2\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"in\": 12, \"out\": 14 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error)) << error;
    ASSERT_EQ(playlist.clips().size(), 3u);
    ASSERT_EQ(playlist.length(), 18);
    EXPECT_EQ(playlist.in(), 0);
    EXPECT_EQ(playlist.out(), 17);

    const heisenberg::ProducerFrame first = playlist.getFrame(0);
    const heisenberg::ProducerFrame beforeCut = playlist.getFrame(9);
    const heisenberg::ProducerFrame afterCut = playlist.getFrame(10);
    const heisenberg::ProducerFrame beforeReturn = playlist.getFrame(14);
    const heisenberg::ProducerFrame returned = playlist.getFrame(15);
    const heisenberg::ProducerFrame last = playlist.getFrame(17);
    ASSERT_FALSE(first.eof || beforeCut.eof || afterCut.eof ||
                 beforeReturn.eof || returned.eof || last.eof);
    ASSERT_TRUE(first.hasVideo());
    ASSERT_TRUE(afterCut.hasVideo());
    ASSERT_TRUE(returned.hasVideo());
    EXPECT_EQ(first.position, 0);
    EXPECT_EQ(afterCut.position, 10);
    EXPECT_EQ(last.position, 17);
    EXPECT_TRUE(isCanvas(first));
    EXPECT_TRUE(isCanvas(beforeCut));
    EXPECT_TRUE(isCanvas(afterCut));
    EXPECT_TRUE(isCanvas(beforeReturn));
    EXPECT_TRUE(isCanvas(returned));
    EXPECT_TRUE(isCanvas(last));

    for (int64_t index = 0; index < playlist.length(); ++index) {
        const heisenberg::ProducerFrame frame = playlist.getFrame(index);
        ASSERT_FALSE(frame.eof) << index;
        ASSERT_TRUE(frame.hasVideo()) << index;
        EXPECT_EQ(frame.position, index);
    }

    const heisenberg::ProducerFrame pastEnd = playlist.getFrame(playlist.length());
    EXPECT_TRUE(pastEnd.eof);
}
