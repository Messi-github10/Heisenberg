#include <Producer/Producer.hpp>
#include <Utiles/Logger.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

std::string utf8FromPath(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

struct LoggerGuard {
    LoggerGuard() { heisenberg::Logger::Init(); }
    ~LoggerGuard() { heisenberg::Logger::Shutdown(); }
};

const LoggerGuard kLoggerGuard;

} // namespace

TEST(ProducerTest, GetFrameAndSeekUseProfileCanvas) {
    const std::filesystem::path media = std::filesystem::u8path(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(std::filesystem::exists(media)) << utf8FromPath(media);

    heisenberg::Producer producer(heisenberg::Profile::hd1080p24());
    producer.setHardwareDecode(false);
    std::string error;
    ASSERT_TRUE(producer.open(utf8FromPath(media), &error)) << error;
    ASSERT_GT(producer.length(), 0);
    EXPECT_EQ(producer.in(), 0);
    EXPECT_EQ(producer.out(), producer.length() - 1);

    const heisenberg::ProducerFrame first = producer.getFrame(0);
    const heisenberg::ProducerFrame second = producer.getFrame(1);
    const heisenberg::ProducerFrame third = producer.getFrame(2);
    ASSERT_FALSE(first.eof);
    ASSERT_FALSE(second.eof);
    ASSERT_FALSE(third.eof);
    ASSERT_TRUE(first.hasVideo());
    ASSERT_TRUE(second.hasVideo());
    ASSERT_TRUE(third.hasVideo());
    EXPECT_EQ(first.position, 0);
    EXPECT_EQ(second.position, 1);
    EXPECT_EQ(third.position, 2);
    EXPECT_EQ(first.video->width, 1920);
    EXPECT_EQ(first.video->height, 1080);
    EXPECT_EQ(first.video->format, AV_PIX_FMT_RGBAF16);
    if (first.hasAudio()) {
        EXPECT_EQ(first.audio->spec().sampleRate, 48000);
        EXPECT_EQ(first.audio->spec().channels, 2);
        EXPECT_EQ(first.audio->samples(), 2000);
    }

    const int64_t lastIndex = producer.length() - 1;
    const heisenberg::ProducerFrame last = producer.getFrame(lastIndex);
    ASSERT_FALSE(last.eof);
    ASSERT_TRUE(last.hasVideo());
    EXPECT_EQ(last.position, lastIndex);

    const int64_t jumpIndex = std::min<int64_t>(10, lastIndex);
    ASSERT_TRUE(producer.seek(jumpIndex));
    const heisenberg::ProducerFrame jumped = producer.getFrame(jumpIndex);
    ASSERT_FALSE(jumped.eof);
    ASSERT_TRUE(jumped.hasVideo());
    EXPECT_EQ(jumped.position, jumpIndex);

    const heisenberg::ProducerFrame pastEnd = producer.getFrame(producer.length());
    EXPECT_TRUE(pastEnd.eof);
    producer.close();
}
