#include "AudioMixer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace heisenberg {
std::shared_ptr<AudioFrame> AudioMixer::render(const AudioSpec& spec, int samples,
    int64_t targetSamplePosition, const std::vector<Input>& inputs) {
    if (!spec.valid() || samples < 0) return {};
    auto output = std::make_shared<AudioFrame>();
    output->setSpec(spec);
    output->setSamples(samples, true);
    output->setPosition(targetSamplePosition);
    output->clear();
    for (const auto& input : inputs) {
        if (!input.source || input.muted || !std::isfinite(input.sourceSeconds)
            || !std::isfinite(input.speed) || input.speed <= 0 || !std::isfinite(input.gain)
            || !std::isfinite(input.clipOffsetSeconds) || !std::isfinite(input.clipDurationSeconds)
            || !std::isfinite(input.fadeInSeconds) || !std::isfinite(input.fadeOutSeconds)
            || input.clipDurationSeconds < 0 || input.fadeInSeconds < 0 || input.fadeOutSeconds < 0) continue;
        auto sample = [&](int channel, int64_t position) {
            for (const auto& frame : *input.source) {
                if (!frame || !frame->hasPosition() || frame->spec() != spec
                    || frame->samples() <= 0 || frame->empty() || position < frame->position()) continue;
                // Unsigned subtraction also supports negative source timestamps
                // without overflowing when an unrelated frame is far away.
                const uint64_t relative = static_cast<uint64_t>(position) - static_cast<uint64_t>(frame->position());
                if (relative < static_cast<uint64_t>(frame->samples()))
                    return frame->sampleRo(channel, static_cast<int>(relative));
            }
            return 0.0f;
        };
        const double start = input.sourceSeconds * spec.sampleRate;
        for (int s = 0; s < samples; ++s) {
            const double time = input.clipOffsetSeconds + static_cast<double>(s) / spec.sampleRate;
            if (input.clipDurationSeconds > 0 && (time < 0 || time >= input.clipDurationSeconds)) continue;
            double gain = input.gain;
            if (input.fadeInSeconds > 0)
                gain *= std::clamp(time / input.fadeInSeconds, 0.0, 1.0);
            if (input.fadeOutSeconds > 0)
                gain *= std::clamp((input.clipDurationSeconds - time) / input.fadeOutSeconds, 0.0, 1.0);
            const double position = start + s * input.speed;
            if (!std::isfinite(position)
                || position < static_cast<double>(std::numeric_limits<int64_t>::min())
                || position >= static_cast<double>(std::numeric_limits<int64_t>::max())) continue;
            const auto lower = static_cast<int64_t>(std::floor(position));
            const double fraction = position - lower;
            for (int c = 0; c < spec.channels; ++c) {
                const double value = sample(c, lower) * (1.0 - fraction) + sample(c, lower + 1) * fraction;
                output->sampleRw(c, s) += static_cast<float>(value * gain);
            }
        }
    }
    for (int c = 0; c < spec.channels; ++c)
        for (int s = 0; s < samples; ++s)
            output->sampleRw(c, s) = std::clamp(output->sampleRo(c, s), -1.0f, 1.0f);
    return output;
}
std::shared_ptr<AudioFrame> AudioMixer::render(const AudioSpec& spec, int samples,
    int64_t targetSamplePosition, const std::vector<std::shared_ptr<AudioFrame>>& source,
    double sourceSeconds, double speed, double gain, bool muted) {
    return render(spec, samples, targetSamplePosition, {Input{&source, sourceSeconds, speed, gain, muted}});
}
} // namespace heisenberg
