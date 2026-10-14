#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace heisenberg::renderer {

// The application's single render worker. Accepted commands are never replaced
// or cancelled: stop rejects new work and drains the FIFO before joining.
class RenderThread {
public:
    using Task = std::function<void()>;
    // Returns milliseconds until the next clock tick; negative stops playback.
    using ClockTask = std::function<int()>;

    RenderThread();
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start(ClockTask clock = {});
    void stop();
    // Throws std::runtime_error when no worker is accepting commands.
    void post(Task task);
    // Propagates task exceptions. A call from the worker executes inline.
    void postSync(Task task);
    void setPlaying(bool playing);
    bool isCurrentThread() const;
    bool isRunning() const;
    // Asynchronous command/clock failures do not kill the worker. The owner can
    // retrieve them here; synchronous failures are delivered to their caller.
    std::exception_ptr takeException();

private:
    void run();
    void recordException(std::exception_ptr exception);

    std::thread thread_;
    // Serializes external start/join, never held by a worker-side stop.
    std::mutex lifecycleMutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> pending_;
    std::deque<std::exception_ptr> exceptions_;
    ClockTask clock_;
    std::thread::id workerId_;
    bool running_ = false;
    bool playing_ = false;
    // Every explicit clock-state request invalidates an in-flight clock result
    // and its deadline, even setPlaying(true) while already playing.
    uint64_t clockEpoch_ = 0;
};

} // namespace heisenberg::renderer
