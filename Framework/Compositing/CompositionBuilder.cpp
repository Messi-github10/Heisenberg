#include "CompositionBuilder.hpp"
#include <Models/MediaResolver.hpp>
#include <algorithm>

namespace heisenberg {

bool CompositionBuilder::build(const Timeline& timeline, const MediaManifest& manifest,
                               const std::string& projectDir, CompositionPlan& result,
                               std::string* error) {
    if (!validateTimeline(timeline, error)) return false;
    const auto compatibility = compatibilityFor(timeline);
    if (compatibility.level == CompatibilityLevel::Unsupported) {
        if (error) *error = compatibility.messages.empty() ? "unsupported timeline"
                                                         : compatibility.messages.front();
        return false;
    }
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(timeline.width, timeline.height, timeline.fps);
    plan.frameCount = timeline.totalFrames();
    plan.warnings = compatibility.messages;
    if (!timeline.tracks.empty()) {
        const auto& track = timeline.tracks.front();
        plan.hidden = track.hidden;
        plan.muted = track.muted;
        MediaResolver resolver(manifest, projectDir);
        for (const auto& clip : track.clips) {
            CompositionClip value;
            value.id = clip.id;
            value.mediaRef = clip.mediaRef;
            value.sourcePath = resolver.expectedPath(clip.mediaRef);
            value.startFrame = clip.startFrame;
            value.durationFrames = clip.durationFrames;
            value.trimStartFrame = clip.trimStartFrame;
            value.trimEndFrame = clip.trimEndFrame;
            value.speed = clip.speed;
            value.volume = clip.volume;
            value.fadeInFrames = clip.fadeInFrames;
            value.fadeOutFrames = clip.fadeOutFrames;
            if (value.sourcePath.empty()) plan.warnings.push_back("offline media: " + clip.mediaRef);
            plan.clips.push_back(std::move(value));
        }
        std::sort(plan.clips.begin(), plan.clips.end(),
            [](const auto& left, const auto& right) { return left.startFrame < right.startFrame; });
    }
    result = std::move(plan);
    return true;
}

} // namespace heisenberg
