#pragma once
#include <Common/AudioFrame.hpp>
#include <memory>
#include <vector>

namespace heisenberg {

// Single-track timeline audio mapping, not a multitrack compositor. Missing
// samples/gaps are silence. Speed changes resample time (including pitch).
class AudioMixer {
public:
    static std::shared_ptr<AudioFrame> render(const AudioSpec& spec, int samples,
        int64_t targetSamplePosition, const std::vector<std::shared_ptr<AudioFrame>>& source,
        double sourceSeconds, double speed = 1.0, double gain = 1.0, bool muted = false);
};

} // namespace heisenberg
