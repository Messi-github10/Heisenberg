//
// Created by NiceFold on 2026/7/27.
//

#pragma once

#include <Common/AudioSpec.hpp>
#include <Common/NonCopy.hpp>
#include <cstdint>
#include <functional>
#include <memory>

struct ma_device;

namespace heisenberg {
namespace renderer {

class AudioDevice : public NonCopy {
public:
    using AudioCallback = std::function<void(float* output, uint32_t frameCount)>;

    AudioDevice();
    ~AudioDevice();

    bool initialize(const AudioSpec& spec, AudioCallback callback);

    bool start();

    bool stop();

    bool isPlaying() const;

    void setVolume(float volume);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
