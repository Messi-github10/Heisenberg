#include "CompositionPlan.hpp"
#include <algorithm>

namespace heisenberg {

std::optional<CompositionSample> CompositionPlan::sampleAt(int64_t frame) const {
    if (frame < 0 || frame >= frameCount) return {};
    const auto next = std::upper_bound(clips.begin(), clips.end(), frame,
        [](int64_t position, const CompositionClip& clip) { return position < clip.startFrame; });
    if (next == clips.begin()) return {};
    const auto found = std::prev(next);
    const int64_t relative = frame - found->startFrame;
    if (relative < 0 || relative >= found->durationFrames) return {};
    CompositionSample result;
    result.clipIndex = static_cast<size_t>(found - clips.begin());
    // Trim offsets are in timeline-rate units, not the decoder's native rate.
    // Keep fractional positions until the decoder maps seconds to native PTS.
    result.sourceSeconds = settings.secondsFromFrame(found->trimStartFrame)
        + settings.secondsFromFrame(relative) * found->speed;
    result.speed = found->speed;
    result.gain = found->volume;
    if (found->fadeInFrames > 0)
        result.gain *= std::min(1.0, static_cast<double>(relative) / found->fadeInFrames);
    if (found->fadeOutFrames > 0)
        result.gain *= std::min(1.0, static_cast<double>(found->durationFrames - relative - 1)
                                   / found->fadeOutFrames);
    result.hidden = hidden;
    result.muted = muted;
    return result;
}

} // namespace heisenberg
