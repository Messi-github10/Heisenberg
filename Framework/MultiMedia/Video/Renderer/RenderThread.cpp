#include "MultiMedia/Video/Renderer/RenderThread.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

namespace heisenberg::renderer {

RenderThread::RenderThread() = default;
RenderThread::~RenderThread() { stop(); }

void RenderThread::start(ClockTask clock) {
    if (isCurrentThread()) {
        throw std::logic_error("Cannot restart RenderThread from its worker");
    }
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
    playing_ = false;
    running_ = true;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        running_ = false;
        clock_ = {};
        throw;
    }
}

void RenderThread::stop() {
    // A task may request shutdown, but cannot join itself. An external stop or
    // destruction subsequently joins the worker after the accepted queue drains.
    if (isCurrentThread()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            playing_ = false;
        }
        cv_.notify_all();
        return;
    }
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        playing_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = {};
}

void RenderThread::post(Task task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) throw std::runtime_error("RenderThread is not accepting commands");
        pending_.push_back(std::move(task));
    }
    cv_.notify_all();
}

void RenderThread::postSync(Task task) {
    if (!task) return;
    if (isCurrentThread()) {
        task();
        return;
    }
    auto command = std::make_shared<std::packaged_task<void()>>(std::move(task));
    auto done = command->get_future();
    // Enqueue and shutdown are serialized by the same mutex. Once accepted,
    // this shared completion survives shutdown and always completes or throws.
    post([command] { (*command)(); });
    done.get();
}

void RenderThread::setPlaying(bool playing) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = running_ && playing;
        ++clockEpoch_;
    }
    cv_.notify_all();
}

bool RenderThread::isCurrentThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workerId_ == std::this_thread::get_id();
}

bool RenderThread::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::exception_ptr RenderThread::takeException() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exceptions_.empty()) return {};
    auto result = exceptions_.front();
    exceptions_.pop_front();
    return result;
}

void RenderThread::recordException(std::exception_ptr exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    exceptions_.push_back(std::move(exception));
}

void RenderThread::run() {
    using Clock = std::chrono::steady_clock;
    auto nextTick = Clock::now();
    uint64_t deadlineEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerId_ = std::this_thread::get_id();
    }
    for (;;) {
        Task task;
        ClockTask tick;
        uint64_t tickEpoch = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (deadlineEpoch != clockEpoch_) {
                nextTick = Clock::now();
                deadlineEpoch = clockEpoch_;
            }
            if (!pending_.empty()) {
                task = std::move(pending_.front());
                pending_.pop_front();
            } else if (!running_) {
                workerId_ = {};
                return;
            } else if (!playing_) {
                cv_.wait(lock, [this] { return !pending_.empty() || playing_ || !running_; });
                nextTick = Clock::now();
                continue;
            } else if (Clock::now() < nextTick) {
                cv_.wait_until(lock, nextTick, [this, deadlineEpoch] {
                    return !pending_.empty() || !playing_ || !running_ ||
                           clockEpoch_ != deadlineEpoch;
                });
                continue;
            } else {
                tick = clock_;
                tickEpoch = clockEpoch_;
            }
        }
        if (task) {
            try { task(); }
            catch (...) { recordException(std::current_exception()); }
        } else {
            int waitMs = -1;
            try { if (tick) waitMs = tick(); }
            catch (...) { recordException(std::current_exception()); }
            std::lock_guard<std::mutex> lock(mutex_);
            deadlineEpoch = clockEpoch_;
            if (tickEpoch != clockEpoch_) {
                // Decoder wake/state change during the unlocked pump wins over
                // both an old idle result and an old positive sleep duration.
                nextTick = Clock::now();
            } else {
                if (waitMs < 0) playing_ = false;
                // A zero-delay clock must not busy-spin or starve other producers.
                nextTick = Clock::now() + std::chrono::milliseconds(std::max(waitMs, 1));
            }
        }
    }
}

} // namespace heisenberg::renderer
