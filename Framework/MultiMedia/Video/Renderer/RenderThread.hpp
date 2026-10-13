#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace heisenberg::renderer {

// Owns every swapchain submit. Commands posted from the UI thread run here,
// and playback sleeps until the next frame is due before presenting it.
class RenderThread {
public:
    using Task = std::function<void()>;
    // Returns the milliseconds to wait. A negative result stops the clock.
    using ClockTask = std::function<int()>;

    RenderThread();
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start(ClockTask clock);
    void stop();

    void post(Task task);
    void postSync(Task task);

    void setPlaying(bool playing);

private:
    void run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    Task pending_;
    ClockTask clock_;
    bool running_ = false;
    bool playing_ = false;
    bool started_ = false;
};

} // namespace heisenberg::renderer
