#include "MultiMedia/Video/Renderer/RenderThread.hpp"

#include <chrono>
#include <utility>

namespace heisenberg::renderer {

RenderThread::RenderThread() = default;

RenderThread::~RenderThread() {
    stop();
}

void RenderThread::start(ClockTask clock) {
    if (started_) return;
    clock_ = std::move(clock);
    running_ = true;
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void RenderThread::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        running_ = false;
        playing_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
    clock_ = nullptr;
}

void RenderThread::post(Task task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(task);
    }
    cv_.notify_all();
}

void RenderThread::postSync(Task task) {
    if (!task) return;
    if (!started_) {
        task();
        return;
    }
    std::mutex doneMutex;
    std::condition_variable done;
    bool finished = false;
    post([&] {
        task();
        {
            std::lock_guard<std::mutex> lock(doneMutex);
            finished = true;
        }
        done.notify_all();
    });
    std::unique_lock<std::mutex> lock(doneMutex);
    done.wait(lock, [&] { return finished; });
}

void RenderThread::setPlaying(bool playing) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = playing;
    }
    cv_.notify_all();
}

void RenderThread::run() {
    while (true) {
        Task task;
        int waitMs = 2;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!pending_ && playing_ && running_) {
                lock.unlock();
                waitMs = clock_ ? clock_() : 2;
                lock.lock();
                if (waitMs < 0) playing_ = false;
            }
            if (!running_ && !pending_) return;
            if (pending_) {
                task = std::move(pending_);
                pending_ = nullptr;
            } else if (!playing_) {
                cv_.wait(lock, [this] { return pending_ || playing_ || !running_; });
                continue;
            } else if (waitMs > 0) {
                cv_.wait_for(lock, std::chrono::milliseconds(waitMs), [this] {
                    return pending_ || !playing_ || !running_;
                });
                continue;
            }
        }
        if (task) task();
    }
}

} // namespace heisenberg::renderer
