#include <Preview/VideoEngine.hpp>
#include <Models/Timeline.hpp>
#include <Models/MediaManifest.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>

using namespace heisenberg;
namespace {
Timeline offlineTimeline(int frames = 9) {
    Timeline value;
    value.id = "offline-timeline"; value.fps = 30;
    Track track; track.id = "video";
    Clip clip; clip.id = "offline"; clip.mediaRef = "missing";
    clip.startFrame = 2; clip.durationFrames = frames - 2;
    track.clips.push_back(clip); value.tracks.push_back(track); return value;
}
template<class Predicate> bool pumpUntil(VideoEngine& engine, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        int wait = 0; engine.pumpFrame(wait);
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
}

TEST(VideoEngineTransportTest, OfflineFirstSeekScrubAndEndUseRenderConsumer) {
    VideoEngine engine;
    engine.setHardwareDecode(false);
    const auto renderThread = std::this_thread::get_id();
    std::vector<DecodedFrame> packets;
    engine.onFrameDecoded = [&](DecodedFrame frame) {
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        packets.push_back(std::move(frame));
    };
    ASSERT_TRUE(engine.openTimeline(offlineTimeline(), MediaManifest{}, ""));
    ASSERT_TRUE(pumpUntil(engine, [&] { return !packets.empty() && engine.snapshot().state == VideoEngine::State::Paused; }));
    EXPECT_TRUE(packets.back().blank);
    EXPECT_FALSE(packets.back().source);
    EXPECT_EQ(packets.back().targetPosition, 0);
    const auto openGeneration = packets.back().generation;
    packets.clear(); engine.seek(4.0 / 30.0);
    ASSERT_TRUE(pumpUntil(engine, [&] { return !packets.empty() && packets.back().targetPosition == 4; }));
    EXPECT_GT(packets.back().generation, openGeneration);
    engine.beginScrub(); engine.scrubToFrame(2); engine.scrubToFrame(7); engine.endScrub(6);
    packets.clear();
    ASSERT_TRUE(pumpUntil(engine, [&] { return !packets.empty() && packets.back().targetPosition == 6
        && engine.snapshot().state == VideoEngine::State::Paused; }));
    engine.play();
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Ended; }));
    EXPECT_NEAR(engine.snapshot().currentTime, 9.0 / 30.0, 1e-6);
    engine.play();
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Playing
        && !packets.empty() && packets.back().targetPosition == 0; }));
    engine.close();
    const auto closed = engine.snapshot().generation;
    packets.clear();
    int wait = 0; engine.pumpFrame(wait);
    EXPECT_TRUE(packets.empty());
    EXPECT_EQ(engine.snapshot().state, VideoEngine::State::Idle);
    EXPECT_EQ(engine.snapshot().generation, closed);
}

TEST(VideoEngineTransportTest, ReopenCancelsQueuedNotificationsAndFrames) {
    VideoEngine engine;
    std::vector<VideoEngine::Task> notifications;
    std::mutex mutex;
    engine.setTaskDispatcher([&](VideoEngine::Task task) { std::lock_guard lock(mutex); notifications.push_back(std::move(task)); });
    int ended = 0;
    engine.onEndOfStream = [&] { ++ended; };
    ASSERT_TRUE(engine.openTimeline(offlineTimeline(4), MediaManifest{}, ""));
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused; }));
    engine.close();
    std::vector<VideoEngine::Task> pending;
    { std::lock_guard lock(mutex); pending.swap(notifications); }
    for (auto& task : pending) task();
    EXPECT_EQ(ended, 0);
    EXPECT_EQ(engine.snapshot().state, VideoEngine::State::Idle);
}
