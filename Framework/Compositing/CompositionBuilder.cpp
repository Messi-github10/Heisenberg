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
    MediaResolver resolver(manifest, projectDir);
    for (const auto& track : timeline.tracks) {
        const bool audio = track.type == ClipType::Audio;
        auto& clips = audio ? plan.audioClips : plan.clips;
        if (audio) { plan.audioTrackId = track.id; plan.audioMuted = track.muted; }
        else { plan.videoTrackId = track.id; plan.hidden = track.hidden; plan.muted = track.muted; }
        for (const auto& clip : track.clips) {
            // compatibilityFor is the single track/source-type gate. Known
            // media must also agree: a relabeled video asset is not audio extraction.
            if (const auto* media = manifest.find(clip.mediaRef); media && media->type != track.type) {
                if (error) *error = "media type does not match its owning track: " + clip.mediaRef;
                return false;
            }
            if (clip.volumeTrack
                || (clip.fadeInFrames > 0 && clip.fadeInInterpolation != Interpolation::Linear)
                || (clip.fadeOutFrames > 0 && clip.fadeOutInterpolation != Interpolation::Linear))
                plan.warnings.push_back("unsupported audio automation on clip " + clip.id
                    + ": using constant volume and linear fades");
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
            clips.push_back(std::move(value));
        }
        std::sort(clips.begin(), clips.end(),
            [](const auto& left, const auto& right) { return left.startFrame < right.startFrame; });
    }
    result = std::move(plan);
    return true;
}

} // namespace heisenberg
