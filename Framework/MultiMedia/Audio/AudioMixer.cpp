#include "AudioMixer.hpp"
#include <algorithm>
#include <cmath>

namespace heisenberg {
std::shared_ptr<AudioFrame> AudioMixer::render(const AudioSpec& spec, int samples,
    int64_t targetSamplePosition, const std::vector<std::shared_ptr<AudioFrame>>& source,
    double sourceSeconds, double speed, double gain, bool muted) {
    if (!spec.valid() || samples < 0) return {};
    auto output = std::make_shared<AudioFrame>();
    output->setSpec(spec);
    output->setSamples(samples, true);
    output->setPosition(targetSamplePosition);
    output->clear();
    if (muted || !std::isfinite(sourceSeconds) || !std::isfinite(speed) || speed <= 0
        || !std::isfinite(gain)) return output;
    auto sample = [&](int channel, int64_t position) {
        for (const auto& frame : source) {
            if (!frame || !frame->hasPosition() || frame->spec().channels != spec.channels
                || frame->spec().sampleRate != spec.sampleRate) continue;
            const int64_t relative = position - frame->position();
            if (relative >= 0 && relative < frame->samples())
                return frame->sampleRo(channel, static_cast<int>(relative));
        }
        return 0.0f;
    };
    const double start = sourceSeconds * spec.sampleRate;
    for (int s = 0; s < samples; ++s) {
        const double position = start + s * speed;
        const auto lower = static_cast<int64_t>(std::floor(position));
        const double fraction = position - lower;
        for (int c = 0; c < spec.channels; ++c) {
            const double value = sample(c, lower) * (1.0 - fraction) + sample(c, lower + 1) * fraction;
            output->sampleRw(c, s) = static_cast<float>(std::clamp(value * gain, -1.0, 1.0));
        }
    }
    return output;
}
} // namespace heisenberg
