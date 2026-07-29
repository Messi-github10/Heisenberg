//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include "AudioSpec.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace heisenberg {

class AudioFrame {
public:
    AudioFrame()  = default;
    ~AudioFrame() = default;

    AudioFrame(const AudioFrame&)            = default;
    AudioFrame& operator=(const AudioFrame&) = default;
    AudioFrame(AudioFrame&&) noexcept        = default;
    AudioFrame& operator=(AudioFrame&&) noexcept = default;

    const AudioSpec& spec() const { return spec_; }

    int samples() const { return samples_; }

    // 此帧在时间轴上的位置。
    int64_t position() const { return position_; }
    bool hasPosition() const { return position_ >= 0; }

    // 可读写原始平面缓冲区指针
    float* dataRw() { return data_.data(); }

    // 只读原始平面缓冲区指针
    const float* dataRo() const { return data_.data(); }

    // 替换音频规格
    void setSpec(const AudioSpec& s) { spec_ = s; }

    // 设置每通道采样数
    void setSamples(int n) { samples_ = n; }

    // 设置此帧在时间轴上的位置。
    void setPosition(int64_t pos) { position_ = pos; }

    // 调整内部缓冲区大小
    void resize(size_t n) { data_.resize(n); }

    /// 将缓冲区调整为 @p n 个 float 并全部填充为 @p v。
    void assign(size_t n, float v) { data_.assign(n, v); }

    // 缓冲区中的 float 总数
    int totalFloats() const { return samples_ * spec_.channels; }

    // 缓冲区的字节总数
    int bytes() const { return totalFloats() * static_cast<int>(sizeof(float)); }

    /// @return 缓冲区是否为空
    bool empty() const { return data_.empty(); }

    /// 获取通道 @p ch 的可读写起始指针
    float* channelRw(int ch) { return data_.data() + ch * samples_; }

    /// 获取通道 @p ch 的只读起始指针
    const float* channelRo(int ch) const { return data_.data() + ch * samples_; }

    /// 获取通道 @p ch 中第 @p s 个采样的可读写引用
    float& sampleRw(int ch, int s) { return channelRw(ch)[s]; }

    /// 读取通道 @p ch 中第 @p s 个采样的值
    float sampleRo(int ch, int s) const { return channelRo(ch)[s]; }

    /// 根据当前 spec 的通道数为 @p sampleCount 个采样预留空间
    void reserve(int sampleCount) {
        data_.resize(static_cast<size_t>(sampleCount) * spec_.channels);
    }

    /// 设置每通道采样数，并可选择同时调整缓冲区大小
    void setSamples(int sampleCount, bool resizeBuffer) {
        samples_ = sampleCount;
        if (resizeBuffer) {
            data_.resize(static_cast<size_t>(sampleCount) * spec_.channels);
        }
    }

    /// 将整个缓冲区填充为零
    void clear() {
        std::fill(data_.begin(), data_.end(), 0.0f);
    }

    /// 将单个通道填充为零
    void clearChannel(int ch) {
        if (ch < spec_.channels) {
            std::fill(channelRw(ch), channelRw(ch) + samples_, 0.0f);
        }
    }

    /// 释放缓冲区并将所有状态重置为默认值
    void reset() {
        data_.clear();
        data_.shrink_to_fit();
        samples_  = 0;
        position_ = -1;
    }

    /// 将另一帧的平面音频数据拷贝到当前帧
    void copyFrom(const AudioFrame& src, int count, int srcStart = 0, int dstStart = 0) {
        if (src.spec_.channels != spec_.channels) return;

        for (int c = 0; c < spec_.channels; ++c) {
            const float* srcPtr = src.channelRo(c) + srcStart;
            float*       dstPtr = channelRw(c) + dstStart;
            std::memcpy(dstPtr, srcPtr, static_cast<size_t>(count) * sizeof(float));
        }
    }

    /// 将平面 float32 转换为交织 float32
    void toInterleaved(float* output, int frameCount) const {
        for (int f = 0; f < frameCount; ++f) {
            for (int c = 0; c < spec_.channels; ++c) {
                output[f * spec_.channels + c] = channelRo(c)[f];
            }
        }
    }

private:
    std::vector<float> data_;
    AudioSpec spec_;
    int samples_ = 0;
    int64_t position_ = -1;
};

} // namespace heisenberg
