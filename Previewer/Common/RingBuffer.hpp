//
// Created by NiceFold on 2026/7/12.
//

#pragma once

#include "NonCopy.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace heisenberg {

template <typename T>
class RingBuffer : public NonCopy {
public:
    using Milliseconds = std::chrono::milliseconds;

    explicit RingBuffer(size_t capacity)
        : capacity_(capacity) {}

    ~RingBuffer() {
        shutdown();
    }

    // Producer
    bool push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFullCond_.wait(lock, [this] {
            return buffer_.size() < capacity_ || abort_ || shutdown_;
        });
        if (abort_ || shutdown_) return false;
        buffer_.push_back(item);
        notEmptyCond_.notify_all();
        return true;
    }

    bool pushFront(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFullCond_.wait(lock, [this] {
            return buffer_.size() < capacity_ || abort_ || shutdown_;
        });
        if (abort_ || shutdown_) return false;
        buffer_.push_front(item);
        notEmptyCond_.notify_all();
        return true;
    }

    bool pushWithTimeout(const T& item, Milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ok = notFullCond_.wait_for(lock, timeout, [this] {
            return buffer_.size() < capacity_ || abort_ || shutdown_;
        });
        if (!ok || abort_ || shutdown_) return false;
        buffer_.push_back(item);
        notEmptyCond_.notify_all();
        return true;
    }

    // Consumer
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmptyCond_.wait(lock, [this] {
            return !buffer_.empty() || abort_ || shutdown_;
        });
        if (abort_ || shutdown_ || buffer_.empty()) return false;
        item = std::move(buffer_.front());
        buffer_.pop_front();
        notFullCond_.notify_all();
        return true;
    }

    bool popWithTimeout(T& item, Milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ok = notEmptyCond_.wait_for(lock, timeout, [this] {
            return !buffer_.empty() || abort_ || shutdown_;
        });
        if (!ok || abort_ || shutdown_ || buffer_.empty()) return false;
        item = std::move(buffer_.front());
        buffer_.pop_front();
        notFullCond_.notify_all();
        return true;
    }

    // 批量弹出队列头部元素，直到满足指定条件为止
    template <typename Predicate>
    std::optional<T> popUntil(Predicate pred) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<T> lastPopped;

        while (!buffer_.empty() && !pred(buffer_.front())) {
            lastPopped = std::move(buffer_.front());
            buffer_.pop_front();
        }

        if (lastPopped.has_value()) {
            notFullCond_.notify_all();
        }

        return lastPopped;
    }

    void abort() {
        std::unique_lock<std::mutex> lock(mutex_);
        abort_ = true;
        buffer_.clear();
        notEmptyCond_.notify_all();
        notFullCond_.notify_all();
    }

    // Wake blocked producers/consumers without discarding buffered items.
    void interrupt() {
        std::unique_lock<std::mutex> lock(mutex_);
        abort_ = true;
        notEmptyCond_.notify_all();
        notFullCond_.notify_all();
    }

    void resume() {
        std::unique_lock<std::mutex> lock(mutex_);
        abort_ = false;
    }

    bool isAborted() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return abort_;
    }

    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        buffer_.clear();
        notFullCond_.notify_all();
    }

    void shutdown() {
        std::unique_lock<std::mutex> lock(mutex_);
        shutdown_ = true;
        buffer_.clear();
        notEmptyCond_.notify_all();
        notFullCond_.notify_all();
    }

    bool isShutdown() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return shutdown_;
    }

    std::optional<T> peekFront() const {
        std::unique_lock<std::mutex> lock(mutex_);
        if (buffer_.empty()) return std::nullopt;
        return buffer_.front();
    }

    bool empty() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return buffer_.empty();
    }

    bool full() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return buffer_.size() >= capacity_;
    }

    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable notEmptyCond_;
    std::condition_variable notFullCond_;
    std::deque<T>           buffer_;
    size_t                  capacity_;
    bool                    abort_    = false;
    bool                    shutdown_ = false;
};

} // namespace heisenberg
