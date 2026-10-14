#include <MultiMedia/Video/Renderer/RenderThread.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <vector>

using heisenberg::renderer::RenderThread;

TEST(RenderThreadTest, CommandsPreserveFifoAndThreadAffinity) {
    RenderThread worker;
    worker.start();
    std::vector<int> seen;
    bool onWorker = true;
    for (int index = 0; index < 256; ++index) {
        worker.post([&, index] {
            onWorker = onWorker && worker.isCurrentThread();
            seen.push_back(index);
        });
    }
    worker.postSync([] {});
    ASSERT_TRUE(onWorker);
    ASSERT_EQ(seen.size(), 256u);
    for (int index = 0; index < 256; ++index) EXPECT_EQ(seen[index], index);
}

TEST(RenderThreadTest, NestedSynchronousCallExecutesInline) {
    RenderThread worker;
    worker.start();
    int calls = 0;
    worker.postSync([&] {
        ++calls;
        worker.postSync([&] { ++calls; });
    });
    EXPECT_EQ(calls, 2);
}

TEST(RenderThreadTest, SynchronousExceptionDoesNotKillWorker) {
    RenderThread worker;
    worker.start();
    EXPECT_THROW(worker.postSync([] { throw std::runtime_error("task failure"); }),
                 std::runtime_error);
    bool completed = false;
    worker.postSync([&] { completed = true; });
    EXPECT_TRUE(completed);
}

TEST(RenderThreadTest, StopDrainsAcceptedCommandsAndRejectsNewWork) {
    RenderThread worker;
    worker.start();
    std::atomic<int> calls{0};
    for (int index = 0; index < 128; ++index) worker.post([&] { ++calls; });
    worker.stop();
    EXPECT_EQ(calls.load(), 128);
    EXPECT_FALSE(worker.isRunning());
    EXPECT_THROW(worker.post([] {}), std::runtime_error);
    EXPECT_THROW(worker.postSync([] {}), std::runtime_error);
}

TEST(RenderThreadTest, AsyncFailureCanBeRetrievedAndLaterCommandsComplete) {
    RenderThread worker;
    worker.start();
    worker.post([] { throw std::runtime_error("async failure"); });
    worker.postSync([] {});
    const auto error = worker.takeException();
    ASSERT_NE(error, nullptr);
    EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
    EXPECT_EQ(worker.takeException(), nullptr);
}

namespace {

// Hold the unlocked first pump on a barrier, issue the decoder wake, and only
// then allow its stale result to return. No sleeps or timing guesses establish
// the race; the bounded future wait merely prevents a broken worker hanging tests.
void expectWakeDuringClockIsRetained(int staleResult) {
    std::promise<void> firstEntered;
    auto entered = firstEntered.get_future();
    std::promise<void> releaseFirst;
    auto release = releaseFirst.get_future().share();
    std::promise<void> nextEntered;
    auto next = nextEntered.get_future();
    int calls = 0;
    RenderThread worker;
    worker.start([&] {
        if (++calls == 1) {
            firstEntered.set_value();
            release.wait();
            return staleResult;
        }
        if (calls == 2) nextEntered.set_value();
        return -1;
    });
    worker.setPlaying(true);
    const auto firstStatus = entered.wait_for(std::chrono::seconds(2));
    if (firstStatus == std::future_status::ready) worker.setPlaying(true);
    releaseFirst.set_value();
    const auto nextStatus = next.wait_for(std::chrono::seconds(2));
    worker.stop();
    EXPECT_EQ(firstStatus, std::future_status::ready);
    EXPECT_EQ(nextStatus, std::future_status::ready);
    EXPECT_EQ(calls, 2);
}

} // namespace

TEST(RenderThreadTest, WakeDuringIdleClockResultIsNotLost) {
    expectWakeDuringClockIsRetained(-1);
}

TEST(RenderThreadTest, WakeDuringPositiveClockResultInvalidatesOldDeadline) {
    expectWakeDuringClockIsRetained(60000);
}

TEST(RenderThreadTest, WakeAfterPositiveClockResultInterruptsDeadlineWait) {
    std::promise<void> deadlineComputed;
    auto computed = deadlineComputed.get_future();
    std::promise<void> nextEntered;
    auto next = nextEntered.get_future();
    int calls = 0;
    RenderThread worker;
    worker.start([&] {
        if (++calls == 1) {
            // This FIFO command executes only after this pump result and its
            // deadline have been processed on the same worker.
            worker.post([&] { deadlineComputed.set_value(); });
            return 60000;
        }
        if (calls == 2) nextEntered.set_value();
        return -1;
    });
    worker.setPlaying(true);
    const auto deadlineStatus = computed.wait_for(std::chrono::seconds(2));
    if (deadlineStatus == std::future_status::ready) worker.setPlaying(true);
    const auto nextStatus = next.wait_for(std::chrono::seconds(2));
    worker.stop();
    EXPECT_EQ(deadlineStatus, std::future_status::ready);
    EXPECT_EQ(nextStatus, std::future_status::ready);
    EXPECT_EQ(calls, 2);
}
