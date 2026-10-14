#pragma once
#include <Common/AudioFrame.hpp>
#include <memory>
#include <vector>

namespace heisenberg {

// Each input maps independently from output sample time. Missing samples are
// silence; speed changes resample time (including pitch). All inputs are summed
// before the single output clamp, so opposite-polarity signals cancel correctly.
class AudioMixer {
public:
    struct Input {
        const std::vector<std::shared_ptr<AudioFrame>>* source = nullptr;
        double sourceSeconds = 0.0;
        double speed = 1.0;
        double gain = 1.0; // Base linear volume, without any pre-applied fade.
        bool muted = false;
        // Timeline seconds, independent of source speed. A positive duration
        // gates the input to [0,duration); zero preserves the legacy unbounded API.
        // Input frames must already be resampled to the output AudioSpec.
        double clipOffsetSeconds = 0.0;
        double clipDurationSeconds = 0.0;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
    };
    static std::shared_ptr<AudioFrame> render(const AudioSpec& spec, int samples,
        int64_t targetSamplePosition, const std::vector<Input>& inputs);
    static std::shared_ptr<AudioFrame> render(const AudioSpec& spec, int samples,
        int64_t targetSamplePosition, const std::vector<std::shared_ptr<AudioFrame>>& source,
        double sourceSeconds, double speed = 1.0, double gain = 1.0, bool muted = false);
};

} // namespace heisenberg
