//
// Created by NiceFold on 2026/7/27.
//

#include <Renderer/AudioDevice.hpp>

extern "C" {
#include <miniaudio.h>
}

#include <cstring>

namespace heisenberg {
namespace renderer {

struct AudioDevice::Impl {
    ma_device device{};
    AudioCallback callback;
    AudioSpec spec;
    bool initialized = false;
    bool playing = false;

    static void onAudioData(ma_device* pDevice, void* pOutput, const void* /*pInput*/,
                            ma_uint32 frameCount) {
        auto* impl = static_cast<Impl*>(pDevice->pUserData);

        if (impl->callback) {
            const size_t byteCount = frameCount * impl->spec.channels * sizeof(float);
            std::memset(pOutput, 0, byteCount);

            impl->callback(static_cast<float*>(pOutput), frameCount);
        } else {
            const size_t byteCount = frameCount * impl->spec.channels * sizeof(float);
            std::memset(pOutput, 0, byteCount);
        }
    }

    void close() {
        if (playing) {
            ma_device_stop(&device);
            playing = false;
        }
        if (initialized) {
            ma_device_uninit(&device);
            initialized = false;
        }
        callback = nullptr;
    }
};

AudioDevice::AudioDevice()
    : impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
    impl_->close();
}

bool AudioDevice::initialize(const AudioSpec& spec, AudioCallback callback) {
    impl_->close();

    if (!spec.valid()) return false;
    if (!callback) return false;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);

    config.playback.format   = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(spec.channels);
    config.sampleRate        = static_cast<ma_uint32>(spec.sampleRate);
    config.dataCallback      = Impl::onAudioData;
    config.pUserData         = impl_.get();

    ma_result result = ma_device_init(nullptr, &config, &impl_->device);
    if (result != MA_SUCCESS) {
        return false;
    }

    impl_->spec        = spec;
    impl_->callback    = std::move(callback);
    impl_->initialized = true;
    impl_->playing     = false;

    return true;
}

bool AudioDevice::start() {
    if (!impl_->initialized) return false;

    ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) return false;

    impl_->playing = true;
    return true;
}

bool AudioDevice::stop() {
    if (!impl_->initialized || !impl_->playing) return false;

    ma_result result = ma_device_stop(&impl_->device);
    impl_->playing = false;

    return result == MA_SUCCESS;
}

bool AudioDevice::isPlaying() const {
    return impl_->playing;
}

void AudioDevice::setVolume(float volume) {
    if (impl_->initialized) {
        ma_device_set_master_volume(&impl_->device, volume);
    }
}

} // namespace renderer
} // namespace heisenberg
