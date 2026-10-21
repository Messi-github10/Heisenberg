#include "CompositionBuilder.hpp"
#include <Compositing/Grade/LutLibrary.hpp>
#include <Models/MediaResolver.hpp>
#include <Project/VideoProject.hpp>
#include <algorithm>

namespace heisenberg {
namespace {
// Parameter/schema validation belongs to GradeCompiler. Here we only discover
// references without throwing on malformed or future effect payloads.
void pinGradeLuts(const Clip& clip, CompositionPlan& plan) {
    if (!clip.effects || !clip.effects->is_array()) return;
    for (const auto& effect : *clip.effects) {
        if (!effect.is_object()) continue;
        const auto type = effect.find("type");
        if (type == effect.end() || !type->is_string() || type->get_ref<const std::string&>() != "video.colorGrade") continue;
        const auto enabled = effect.find("enabled");
        if (enabled != effect.end() && enabled->is_boolean() && !enabled->get<bool>()) continue;
        const auto params = effect.find("params");
        if (params == effect.end() || !params->is_object()) continue;
        const auto lut = params->find("lut");
        if (lut == params->end() || !lut->is_object()) continue;
        const auto id = lut->find("id");
        if (id == lut->end() || !id->is_string() || id->get_ref<const std::string&>().empty()) continue;
        // Capture every enabled resource reference, including constant-zero
        // intensity. Rendering may bypass that LUT, but plan identity and
        // captured missing/corrupt diagnostics must not depend on intensity.
        const auto& key = id->get_ref<const std::string&>();
        if (plan.gradeLuts.contains(key) || plan.gradeLutErrors.contains(key)) continue;
        std::string error;
        auto asset = grade::LutLibrary::resolveConfigured(key,&error);
        if (asset) plan.gradeLuts.emplace(key,std::move(asset));
        else plan.gradeLutErrors.emplace(key,std::move(error));
    }
}
} // namespace

bool CompositionBuilder::build(const Timeline& timeline, const MediaManifest& manifest,
                               const std::string& projectDir, CompositionPlan& result,
                               std::string* error) {
    return build(timeline, manifest, projectDir, ColorSettings{}, result, error);
}

bool CompositionBuilder::build(const ProjectSnapshot& snapshot, CompositionPlan& result, std::string* error) {
    const auto* timeline = snapshot.activeTimeline();
    if (!timeline) {
        if (error) *error = "snapshot has no active timeline";
        return false;
    }
    return build(*timeline, snapshot.manifest(), snapshot.projectDir(), snapshot.project().colorSettings, result, error);
}

bool CompositionBuilder::build(const Timeline& timeline, const MediaManifest& manifest,
                               const std::string& projectDir, const ColorSettings& colorSettings,
                               CompositionPlan& result, std::string* error) {
    if (!validateTimeline(timeline, error) || !validateColorSettings(colorSettings, error)) return false;

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
    plan.colorSettings = colorSettings;
    if (!supportedColorSettings(colorSettings)) {
        plan.warnings.push_back("unsupported color settings version; settings preserved without migration and cannot be executed");
    }
    if (colorSettings.legacyDefaultsApplied) {
        if (colorSettings.inputPolicyVersion == 2 && colorSettings.semanticsVersion == 2)
            plan.warnings.push_back("legacy project without color extension: ordinary SDR defaults (input policy v2, semantics v2) were explicitly applied");
        else
            plan.warnings.push_back("legacy project: saved legacy defaults marker preserved without migrating color settings");
    }

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
            value.visualSnapshot = clip;
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
            if (!audio) pinGradeLuts(clip,plan);
            clips.push_back(std::move(value));
        }

        std::sort(clips.begin(), clips.end(),
            [](const auto& left, const auto& right) { return left.startFrame < right.startFrame; });
    }

    result = std::move(plan);
    return true;
}

} // namespace heisenberg
