#include "CompositionPlan.hpp"
#include <algorithm>
#include <iterator>

namespace heisenberg {
namespace {
std::optional<CompositionSample> sample(const CompositionPlan& plan, size_t trackIndex, int64_t frame) {
    if (frame < 0 || frame >= plan.frameCount) return {};
    const auto& track = plan.tracks[trackIndex];
    const auto& clips = track.clips;
    const auto next = std::upper_bound(clips.begin(), clips.end(), frame,
        [](int64_t position, const CompositionClip& clip) { return position < clip.startFrame; });
    if (next == clips.begin()) return {};
    const auto found = std::prev(next);
    const int64_t relative = frame - found->startFrame;
    if (relative < 0 || relative >= found->durationFrames) return {};
    CompositionSample result;
    result.trackIndex = trackIndex;
    result.trackId = track.id;
    result.clipIndex = static_cast<size_t>(found - clips.begin());
    result.sourceSeconds = plan.settings.secondsFromFrame(found->trimStartFrame)
        + plan.settings.secondsFromFrame(relative) * found->speed;
    result.clipLocalFrame = relative;
    result.speed = found->speed;
    result.gain = found->volume;
    result.hidden = track.hidden;
    result.muted = track.muted;
    return result;
}
}

std::vector<CompositionSample> CompositionPlan::videoSamplesAt(int64_t frame) const {
    std::vector<CompositionSample> result;
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index].type == ClipType::Video)
            if (auto value = sample(*this, index, frame)) result.push_back(std::move(*value));
    return result;
}
std::vector<CompositionSample> CompositionPlan::audioSamplesAt(int64_t frame) const {
    std::vector<CompositionSample> result;
    for (size_t index = 0; index < tracks.size(); ++index)
        if (auto value = sample(*this, index, frame)) result.push_back(std::move(*value));
    return result;
}
std::optional<CompositionSample> CompositionPlan::videoSampleAt(int64_t frame) const {
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index].type == ClipType::Video)
            if (auto value = sample(*this, index, frame)) return value;
    return {};
}
std::optional<CompositionSample> CompositionPlan::audioSampleAt(int64_t frame) const {
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index].type == ClipType::Audio)
            if (auto value = sample(*this, index, frame)) return value;
    return {};
}

} // namespace heisenberg
