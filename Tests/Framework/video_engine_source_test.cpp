#include <Preview/VideoEngine.hpp>
#include <Utiles/Logger.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

namespace {

struct LoggerGuard {
    LoggerGuard() { heisenberg::Logger::Init(); }
    ~LoggerGuard() { heisenberg::Logger::Shutdown(); }
};
const LoggerGuard kLoggerGuard;

std::optional<heisenberg::DecodedFrame> waitForFrame(heisenberg::VideoEngine& engine) {
    std::optional<heisenberg::DecodedFrame> result;
    engine.onFrameDecoded = [&](heisenberg::DecodedFrame frame) { result = std::move(frame); };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        int waitMs = 0;
        engine.pumpFrame(waitMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.onFrameDecoded = {};
    return result;
}

} // namespace

TEST(VideoEngineSourceTest, OpenSeekAndScrubKeepNativeFrameMetadata) {
    const std::string path = HEISENBERG_TEST_MEDIA;
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(path)));
    heisenberg::VideoEngine engine;
    engine.setHardwareDecode(false);
    ASSERT_TRUE(engine.open(path));
    const auto first = waitForFrame(engine);
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first->source, nullptr);
    EXPECT_TRUE(first->sourceMode);
    EXPECT_FALSE(first->blank);
    EXPECT_EQ(first->source->spec().width, 320);
    EXPECT_EQ(first->source->spec().height, 240);
    EXPECT_EQ(first->targetPosition, 0);
    EXPECT_GT(engine.snapshot().duration, 0.0);
    EXPECT_TRUE(engine.snapshot().seekable);

    engine.seek(10.0 / 24.0);
    const auto sought = waitForFrame(engine);
    ASSERT_TRUE(sought.has_value());
    ASSERT_NE(sought->source, nullptr);
    EXPECT_NEAR(engine.snapshot().currentTime, 10.0 / 24.0, 1.0 / 24.0);
    EXPECT_GT(sought->source->pts(), first->source->pts());
    EXPECT_EQ(first->source->get()->pts, first->source->pts());

    engine.beginScrub();
    engine.scrubToFrame(2);
    const auto scrubbed = waitForFrame(engine);
    ASSERT_TRUE(scrubbed.has_value());
    EXPECT_EQ(scrubbed->targetPosition, 2);
    engine.endScrub(3);
    const auto settled = waitForFrame(engine);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->targetPosition, 3);
    EXPECT_EQ(engine.snapshot().state, heisenberg::VideoEngine::State::Paused);
    engine.close();
    EXPECT_EQ(engine.snapshot().state, heisenberg::VideoEngine::State::Idle);
}
