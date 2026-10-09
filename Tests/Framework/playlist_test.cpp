#include <Producer/IProducer.hpp>
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

bool firstPixelIs(const heisenberg::ProducerFrame& frame, uint16_t value) {
    if (!isCanvas(frame) || !frame.video->data[0]) return false;
    const uint16_t* pixel = reinterpret_cast<const uint16_t*>(frame.video->data[0]);
    return pixel[0] == value && pixel[1] == value &&
           pixel[2] == value && pixel[3] == value;
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
        "\", \"start\": 0, \"in\": 0, \"out\": 9 },\n" +
        "    { \"id\": \"b1\", \"resource\": \"" + jsonEscape(clipB) +
        "\", \"start\": 10, \"in\": 0, \"out\": 4 },\n" +
        "    { \"id\": \"a2\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 15, \"in\": 12, \"out\": 14 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error)) << error;
    ASSERT_EQ(playlist.clips().size(), 3u);
    ASSERT_EQ(playlist.length(), 18);
    EXPECT_EQ(playlist.clips()[0].start, 0);
    EXPECT_EQ(playlist.clips()[1].start, 10);
    EXPECT_EQ(playlist.clips()[2].start, 15);
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

TEST(PlaylistTest, GapWithoutClipIsBlackCanvas) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    const std::string clipB = HEISENBERG_PLAYLIST_CLIP_B;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipB)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 0, \"in\": 0, \"out\": 4 },\n" +
        "    { \"id\": \"b\", \"resource\": \"" + jsonEscape(clipB) +
        "\", \"start\": 8, \"in\": 0, \"out\": 4 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error)) << error;
    ASSERT_EQ(playlist.clips().size(), 2u);
    ASSERT_EQ(playlist.length(), 13);
    EXPECT_EQ(playlist.clips()[0].start, 0);
    EXPECT_EQ(playlist.clips()[1].start, 8);
    EXPECT_FALSE(playlist.isBlankAt(4));
    EXPECT_FALSE(playlist.isBlankAt(5));
    EXPECT_FALSE(playlist.isBlankAt(7));
    EXPECT_FALSE(playlist.isBlankAt(8));

    const heisenberg::ProducerFrame beforeGap = playlist.getFrame(4);
    const heisenberg::ProducerFrame gapStart = playlist.getFrame(5);
    const heisenberg::ProducerFrame gapEnd = playlist.getFrame(7);
    const heisenberg::ProducerFrame afterGap = playlist.getFrame(8);
    ASSERT_FALSE(beforeGap.eof || gapStart.eof || gapEnd.eof || afterGap.eof);
    EXPECT_TRUE(isCanvas(beforeGap));
    EXPECT_TRUE(isCanvas(gapStart));
    EXPECT_TRUE(isCanvas(gapEnd));
    EXPECT_TRUE(isCanvas(afterGap));
    EXPECT_EQ(gapStart.position, 5);
    EXPECT_EQ(gapEnd.position, 7);
    EXPECT_TRUE(firstPixelIs(gapStart, 0x0000));
    EXPECT_TRUE(firstPixelIs(gapEnd, 0x0000));
    ASSERT_TRUE(gapStart.hasAudio());
    EXPECT_EQ(gapStart.audio->samples(), 2000);
    EXPECT_EQ(gapStart.audio->channelRo(0)[0], 0.0f);

    const heisenberg::ProducerFrame pastEnd = playlist.getFrame(playlist.length());
    EXPECT_TRUE(pastEnd.eof);
}

TEST(PlaylistTest, BlankClipStillProducesWhiteCanvas) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 0, \"in\": 0, \"out\": 4 },\n" +
        "    { \"id\": \"gap\", \"resource\": \"blank\", \"start\": 5, \"in\": 0, \"out\": 2 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error)) << error;
    ASSERT_EQ(playlist.clips().size(), 2u);
    ASSERT_EQ(playlist.length(), 8);
    EXPECT_EQ(playlist.clips()[1].resource, heisenberg::kBlankResource);
    EXPECT_EQ(playlist.clips()[1].start, 5);
    EXPECT_FALSE(playlist.isBlankAt(4));
    EXPECT_TRUE(playlist.isBlankAt(5));
    EXPECT_TRUE(playlist.isBlankAt(7));

    const heisenberg::ProducerFrame white = playlist.getFrame(5);
    ASSERT_FALSE(white.eof);
    EXPECT_TRUE(firstPixelIs(white, 0x3C00));
}

TEST(PlaylistTest, LengthIsLastClipEndAndLeadingGapIsBlack) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 24, \"in\": 0, \"out\": 4 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error)) << error;
    ASSERT_EQ(playlist.clips().size(), 1u);
    ASSERT_EQ(playlist.length(), 29);
    EXPECT_EQ(playlist.clips()[0].start, 24);

    const heisenberg::ProducerFrame leading = playlist.getFrame(0);
    const heisenberg::ProducerFrame beforeClip = playlist.getFrame(23);
    const heisenberg::ProducerFrame firstClip = playlist.getFrame(24);
    ASSERT_FALSE(leading.eof || beforeClip.eof || firstClip.eof);
    EXPECT_TRUE(firstPixelIs(leading, 0x0000));
    EXPECT_TRUE(firstPixelIs(beforeClip, 0x0000));
    EXPECT_TRUE(isCanvas(firstClip));
}

TEST(PlaylistTest, OverlappingClipsAreRejected) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 0, \"in\": 0, \"out\": 9 },\n" +
        "    { \"id\": \"b\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 5, \"in\": 0, \"out\": 4 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    std::string error;
    EXPECT_FALSE(playlist.loadFromJson(json, &error));
    EXPECT_NE(error.find("overlaps"), std::string::npos);
}

TEST(PlaylistTest, FilterInOutUsesTimelineFrames) {
    const std::string clipA = HEISENBERG_PLAYLIST_CLIP_A;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(clipA)));

    const std::string json =
        std::string("{\n") +
        "  \"version\": 1,\n" +
        "  \"profile\": { \"id\": \"hd_1080p_24\" },\n" +
        "  \"clips\": [\n" +
        "    { \"id\": \"a\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 0, \"in\": 0, \"out\": 9 },\n" +
        "    { \"id\": \"b\", \"resource\": \"" + jsonEscape(clipA) +
        "\", \"start\": 15, \"in\": 0, \"out\": 4 }\n" +
        "  ],\n" +
        "  \"filters\": [\n" +
        "    { \"id\": \"grade\", \"graph\": \"brightness.json\", \"in\": 5, \"out\": 12 }\n" +
        "  ]\n" +
        "}\n";

    heisenberg::Playlist playlist;
    playlist.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(playlist.loadFromJson(json, &error, ".")) << error;
    ASSERT_EQ(playlist.length(), 20);
    ASSERT_EQ(playlist.filters().size(), 1u);
    EXPECT_EQ(playlist.filters()[0].id, "grade");
    EXPECT_EQ(playlist.filters()[0].in, 5);
    EXPECT_EQ(playlist.filters()[0].out, 12);
    EXPECT_TRUE(playlist.filters()[0].graph.find("brightness.json") !=
                std::string::npos);

    EXPECT_EQ(playlist.filterAt(4), nullptr);
    ASSERT_NE(playlist.filterAt(5), nullptr);
    EXPECT_EQ(playlist.filterAt(5)->id, "grade");
    ASSERT_NE(playlist.filterAt(12), nullptr);
    EXPECT_EQ(playlist.filterAt(13), nullptr);
    EXPECT_TRUE(playlist.filters()[0].covers(5));
    EXPECT_TRUE(playlist.filters()[0].covers(12));
    EXPECT_FALSE(playlist.filters()[0].covers(4));
}
