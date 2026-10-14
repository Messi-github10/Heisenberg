#include "CompositionPlan.hpp"
#include <algorithm>
#include <iterator>

namespace heisenberg {
namespace {
std::optional<CompositionSample> sample(const CompositionPlan& plan,
    const std::vector<CompositionClip>& clips, int64_t frame, bool hidden, bool muted) {
    if (frame < 0 || frame >= plan.frameCount) return {};
    const auto next = std::upper_bound(clips.begin(), clips.end(), frame,
        [](int64_t position, const CompositionClip& clip) { return position < clip.startFrame; });
    if (next == clips.begin()) return {};
    const auto found = std::prev(next);
    const int64_t relative = frame - found->startFrame;
    if (relative < 0 || relative >= found->durationFrames) return {};
    CompositionSample result;
    result.clipIndex = static_cast<size_t>(found - clips.begin());
    result.sourceSeconds = plan.settings.secondsFromFrame(found->trimStartFrame)
        + plan.settings.secondsFromFrame(relative) * found->speed;
    result.speed = found->speed;
    // The mixer applies clip fades at output-sample resolution, exactly once.
    result.gain = found->volume;
    result.hidden = hidden;
    result.muted = muted;
    return result;
}
}

std::optional<CompositionSample> CompositionPlan::videoSampleAt(int64_t frame) const {
    return sample(*this, clips, frame, hidden, muted);
}
std::optional<CompositionSample> CompositionPlan::audioSampleAt(int64_t frame) const {
    return sample(*this, audioClips, frame, false, audioMuted);
}

} // namespace heisenberg
