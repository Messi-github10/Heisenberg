//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <cstdint>
#include <vector>

namespace heisenberg {

class Packet {
public:
    Packet() = default;
    ~Packet() = default;

    explicit Packet(size_t size) : data_(size) {}

    Packet(const Packet &) = default;
    Packet &operator=(const Packet &) = default;

    Packet(Packet &&) noexcept = default;
    Packet &operator=(Packet &&) noexcept = default;

    uint8_t *data() { return data_.data(); }
    const uint8_t *data() const { return data_.data(); }
    int size() const { return static_cast<int>(data_.size()); }
    bool empty() const { return data_.empty(); }

    void resize(size_t n) { data_.resize(n); }
    void reserve(size_t n) { data_.reserve(n); }
    void clear() { data_.clear(); }

    double pts = -1.0;
    double dts = -1.0;
    double duration = -1.0;
    int streamIndex = -1;
    bool keyframe = false;
    int64_t filePos = -1;

private:
    std::vector<uint8_t> data_;
};

} // namespace heisenberg
