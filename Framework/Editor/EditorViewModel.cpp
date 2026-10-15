#include "EditorViewModel.hpp"

#include <Common/Stream.hpp>
#include <MultiMedia/Demuxer/DemuxerFactory.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <unordered_set>

namespace heisenberg {
namespace {

void setError(std::string* error, const std::string& message) { if (error) *error = message; }

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.c_str()), value.size()};
}

bool addChecked(int64_t left, int64_t right, int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) return false;
    result = left + right;
    return true;
}

} // namespace

struct EditorViewModel::Impl {
    bool hardwareDecode = false;
    std::string packageDir;
    EditorViewModelTimelines timelines;
    MediaManifest manifest;
    uint64_t revision = 0;

    Timeline* active() { return timelines.activeTimeline(); }
    const Timeline* active() const { return timelines.activeTimeline(); }
    Track* track() { return timelines.activeTrack(); }
    const Track* track() const { return timelines.activeTrack(); }
};

namespace {
void shiftJsonKeyframeTracks(JsonValue& value, int64_t shift, int64_t duration);

std::pair<std::optional<KeyframeTrackPayload>, std::optional<KeyframeTrackPayload>>
splitTrackPayload(const std::optional<KeyframeTrackPayload>& source, int64_t offset) {
    if (!source || !source->value.is_object() || !source->value.contains("keyframes") ||
        !source->value["keyframes"].is_array()) return {source, source};
    auto left = source, right = source;
    shiftJsonKeyframeTracks(right->value, -offset, 0);
    return {std::move(left), std::move(right)};
}

std::optional<std::pair<Clip, Clip>> splitValue(const Clip& source, int64_t atFrame) {
    if (atFrame <= source.startFrame || atFrame >= source.endFrame()) return std::nullopt;
    const int64_t offset = atFrame - source.startFrame;
    const int64_t leftSource = std::llround(offset * source.speed);
    const int64_t rightSource = std::llround((source.durationFrames - offset) * source.speed);
    Clip left = source, right = source;
    left.durationFrames = offset;
    left.trimEndFrame += rightSource;
    left.fadeOutFrames = 0;
    right.id = makeUuid();
    right.startFrame = atFrame;
    right.durationFrames = source.durationFrames - offset;
    right.trimStartFrame += leftSource;
    right.fadeInFrames = 0;
#define SPLIT_TRACK(field) { auto pair = splitTrackPayload(source.field, offset); left.field = std::move(pair.first); right.field = std::move(pair.second); }
    SPLIT_TRACK(opacityTrack); SPLIT_TRACK(positionTrack); SPLIT_TRACK(scaleTrack);
    SPLIT_TRACK(rotationTrack); SPLIT_TRACK(cropTrack); SPLIT_TRACK(volumeTrack);
#undef SPLIT_TRACK
    if (source.wordTimings) {
        left.wordTimings = std::vector<WordTiming>{};
        right.wordTimings = std::vector<WordTiming>{};
        for (WordTiming timing : *source.wordTimings) {
            if (timing.startFrame < offset) {
                timing.endFrame = std::min(timing.endFrame, offset);
                if (timing.startFrame < timing.endFrame) left.wordTimings->push_back(timing);
            }
        }
        for (WordTiming timing : *source.wordTimings) {
            timing.startFrame = std::max<int64_t>(timing.startFrame, offset) - offset;
            timing.endFrame -= offset;
            if (timing.startFrame < timing.endFrame && timing.endFrame > 0)
                right.wordTimings->push_back(std::move(timing));
        }
    }
    if (left.effects) shiftJsonKeyframeTracks(*left.effects, 0, left.durationFrames);
    if (right.effects) shiftJsonKeyframeTracks(*right.effects, -offset, right.durationFrames);
    left.clampTimedPayloadsToDuration();
    right.clampTimedPayloadsToDuration();
    return std::make_pair(std::move(left), std::move(right));
}

void setDuration(Clip& clip, int64_t duration) {
    clip.durationFrames = duration;
    clip.clampFadesToDuration();
}

void shiftJsonKeyframeTracks(JsonValue& value, int64_t shift, int64_t duration) {
    if (value.is_array()) {
        for (JsonValue& child : value) shiftJsonKeyframeTracks(child, shift, duration);
        return;
    }
    if (!value.is_object()) return;
    auto keyframes = value.find("keyframes");
    if (keyframes != value.end() && keyframes->is_array()) {
        JsonValue kept = JsonValue::array();
        for (JsonValue keyframe : *keyframes) {
            if (!keyframe.is_object() || !keyframe.contains("frame") || !keyframe["frame"].is_number_integer()) continue;
            int64_t frame = 0;
            // Keep bracketing/history keys on both sides of the visible interval.
            // Rebased negative/future keys preserve exact Hold/Linear evaluation,
            // including unknown payloads, and allow a subsequent trim extension.
            if (!addChecked(keyframe["frame"].get<int64_t>(), shift, frame)) continue;
            keyframe["frame"] = frame;
            kept.push_back(std::move(keyframe));
        }
        *keyframes = std::move(kept);
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "keyframes") shiftJsonKeyframeTracks(it.value(), shift, duration);
    }
}

void shiftTrackPayload(std::optional<KeyframeTrackPayload>& payload, int64_t shift, int64_t duration) {
    if (!payload || !payload->value.is_object() || !payload->value.contains("keyframes") ||
        !payload->value["keyframes"].is_array()) return;
    shiftJsonKeyframeTracks(payload->value, shift, duration);
}

void shiftClipTimedPayloads(Clip& clip, int64_t shift) {
#define SHIFT_TRACK(field) shiftTrackPayload(clip.field, shift, clip.durationFrames)
    SHIFT_TRACK(opacityTrack); SHIFT_TRACK(positionTrack); SHIFT_TRACK(scaleTrack);
    SHIFT_TRACK(rotationTrack); SHIFT_TRACK(cropTrack); SHIFT_TRACK(volumeTrack);
#undef SHIFT_TRACK
    if (clip.effects) shiftJsonKeyframeTracks(*clip.effects, shift, clip.durationFrames);
    if (clip.wordTimings) {
        std::vector<WordTiming> kept;
        for (WordTiming timing : *clip.wordTimings) {
            int64_t start = 0, end = 0;
            if (!addChecked(timing.startFrame, shift, start) || !addChecked(timing.endFrame, shift, end)) continue;
            start = std::max<int64_t>(0, start);
            end = std::min<int64_t>(clip.durationFrames, end);
            if (start >= end) continue;
            timing.startFrame = start;
            timing.endFrame = end;
            kept.push_back(std::move(timing));
        }
        *clip.wordTimings = std::move(kept);
    }
}

std::vector<std::string> clearRegion(Track& track, int64_t start, int64_t end,
                                     const std::unordered_set<std::string>& excluded = {}) {
    std::vector<std::string> changed;
    std::vector<Clip> output;
    for (Clip clip : track.clips) {
        if (excluded.count(clip.id) || clip.endFrame() <= start || clip.startFrame >= end) {
            output.push_back(std::move(clip));
            continue;
        }
        changed.push_back(clip.id);
        if (clip.startFrame >= start && clip.endFrame() <= end) continue;
        if (clip.startFrame < start && clip.endFrame() > end) {
            auto first = splitValue(clip, start);
            auto second = first ? splitValue(first->second, end) : std::nullopt;
            if (first) output.push_back(std::move(first->first));
            if (second) { changed.push_back(second->second.id); output.push_back(std::move(second->second)); }
            continue;
        }
        if (clip.startFrame < start) {
            const int64_t newDuration = start - clip.startFrame;
            clip.trimEndFrame += std::llround((clip.durationFrames - newDuration) * clip.speed);
            setDuration(clip, newDuration);
            shiftClipTimedPayloads(clip, 0);
            output.push_back(std::move(clip));
            continue;
        }
        const int64_t trimAmount = end - clip.startFrame;
        clip.trimStartFrame += std::llround(trimAmount * clip.speed);
        clip.startFrame = end;
        setDuration(clip, clip.durationFrames - trimAmount);
        shiftClipTimedPayloads(clip, -trimAmount);
        output.push_back(std::move(clip));
    }
    track.clips = std::move(output);
    track.sortClips();
    return changed;
}
}

EditorViewModel::EditorViewModel() : impl_(std::make_unique<Impl>()) {}
EditorViewModel::~EditorViewModel() = default;
void EditorViewModel::setHardwareDecode(bool enabled) { impl_->hardwareDecode = enabled; }
void EditorViewModel::setProfile(TimelineSettings settings) { applyTimelineSettings(settings); }
const EditorViewModelTimelines& EditorViewModel::timelines() const { return impl_->timelines; }
uint64_t EditorViewModel::revision() const { return impl_->revision; }

EditResult EditorViewModel::applyTimelineSettings(const TimelineSettings& settings) {
    auto* timeline = impl_->active();
    if (!timeline) return {EditStatus::Invalid, {}, "active timeline is required"};
    Timeline candidate = *timeline;
    std::string error;
    if (!candidate.applySettings(settings, &error)) return {EditStatus::Invalid, {}, error};
    if (candidate == *timeline) return {EditStatus::NoOp, {}, {}};
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Timeline Settings"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {timeline->id}, {}};
}

bool EditorViewModel::importMedia(const std::string& path, std::string* error) {
    const Timeline* timeline = impl_->active();
    if (!timeline || path.empty()) { setError(error, "media path and active timeline are required"); return false; }
    std::error_code code;
    if (!std::filesystem::exists(std::filesystem::u8path(path), code)) {
        setError(error, "media file was not found"); return false;
    }
    auto demux = demuxer::createDemuxer();
    if (demux->open(path) < 0) { setError(error, "Cannot read media file"); return false; }
    const Stream* video = nullptr;
    bool hasAudio = false;
    double streamDuration = 0.0;
    for (const auto& stream : demux->streams()) {
        const bool usableVideo = stream.isVideo() && !stream.attachedPicture;
        if (usableVideo && !video) video = &stream;
        if (stream.isAudio()) hasAudio = true;
        if ((usableVideo || stream.isAudio()) && stream.durationKnown) {
            const double seconds = stream.codec.toSeconds(stream.duration);
            if (std::isfinite(seconds) && seconds > streamDuration) streamDuration = seconds;
        }
    }
    if (!video && !hasAudio) { setError(error, "Media has no usable video or audio stream"); return false; }
    double duration = demux->duration();
    if (!std::isfinite(duration) || duration <= 0.0) duration = streamDuration;
    if (!std::isfinite(duration) || duration <= 0.0) {
        setError(error, "Media duration is unavailable"); return false;
    }
    MediaManifestEntry entry;
    entry.id = makeUuid();
    entry.name = utf8(std::filesystem::u8path(path).stem());
    entry.type = video ? ClipType::Video : ClipType::Audio;
    entry.source = {MediaSource::Kind::External, path};
    entry.duration = duration;
    if (video) {
        entry.sourceWidth = video->codec.width();
        entry.sourceHeight = video->codec.height();
        if (std::isfinite(video->codec.fps()) && video->codec.fps() > 0.0) entry.sourceFPS = video->codec.fps();
    }
    entry.hasAudio = hasAudio;
    if (!impl_->manifest.add(std::move(entry), error)) return false;
    ++impl_->revision;
    return true;
}

const ProjectFile& EditorViewModel::project() const { return impl_->timelines.project(); }
const MediaManifest& EditorViewModel::manifest() const { return impl_->manifest; }
MediaResolver EditorViewModel::resolver() const { return MediaResolver(impl_->manifest, impl_->packageDir); }
const Timeline* EditorViewModel::activeTimeline() const { return impl_->active(); }
const Track* EditorViewModel::activeTrack() const { return impl_->track(); }
const std::string& EditorViewModel::projectDir() const { return impl_->packageDir; }
ProjectCompatibility EditorViewModel::compatibility() const {
    const Timeline* timeline = impl_->active();
    if (!timeline) return {CompatibilityLevel::Unsupported, {"project has no active timeline"}};
    return compatibilityFor(*timeline);
}

std::vector<MediaAsset> EditorViewModel::assets() const {
    std::vector<MediaAsset> result;
    MediaResolver lookup(impl_->manifest, impl_->packageDir);
    for (const MediaManifestEntry& entry : impl_->manifest.entries()) result.push_back(lookup.asset(entry.id));
    return result;
}

EditResult EditorViewModel::placeClip(const std::string& mediaRef, int64_t startFrame,
                                      int64_t durationFrames, int64_t trimStartFrame,
                                      const std::string& targetTrackId) {
    Timeline* timeline = impl_->active();
    const MediaManifestEntry* media = impl_->manifest.find(mediaRef);
    if (!timeline || !media) return {EditStatus::Invalid, {}, "mediaRef was not found"};
    if (media->type != ClipType::Video && media->type != ClipType::Audio)
        return {EditStatus::Unsupported, {}, "only video and audio media can be placed"};
    if (compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    if (!targetTrackId.empty()) {
        const Track* target = timeline->trackById(targetTrackId);
        if (!target) return {EditStatus::Invalid, {}, "target track was not found"};
        if (target->type != media->type) return {EditStatus::Invalid, {}, "media type does not match target track"};
    }
    if (startFrame < 0 || trimStartFrame < 0) return {EditStatus::Invalid, {}, "frame values must be non-negative"};
    const double sourceFrames = media->duration * timeline->fps;
    if (!std::isfinite(sourceFrames) || sourceFrames < 0 ||
        sourceFrames >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return {EditStatus::Invalid, {}, "media duration cannot be represented in timeline frames"};
    const int64_t sourceDuration = std::llround(sourceFrames);
    if (trimStartFrame > sourceDuration) return {EditStatus::Invalid, {}, "clip range exceeds the source duration"};
    if (durationFrames < 0) durationFrames = sourceDuration - trimStartFrame;
    if (durationFrames <= 0) return {EditStatus::Invalid, {}, "clip duration must be positive"};
    if (durationFrames > sourceDuration - trimStartFrame)
        return {EditStatus::Invalid, {}, "clip range exceeds the source duration"};

    int64_t endFrame = 0;
    if (!addChecked(startFrame, durationFrames, endFrame))
        return {EditStatus::Invalid, {}, "clip range overflows"};
    Timeline candidate = *timeline;
    Track* track = targetTrackId.empty() ? nullptr : candidate.trackById(targetTrackId);
    if (!track) {
        for (Track& existing : candidate.tracks) if (existing.type == media->type) { track = &existing; break; }
    }
    if (!track) {
        Track created; created.id = makeUuid(); created.type = media->type;
        if (media->type == ClipType::Video) {
            candidate.tracks.insert(candidate.tracks.begin(), std::move(created));
            track = &candidate.tracks.front();
        } else {
            candidate.tracks.push_back(std::move(created));
            track = &candidate.tracks.back();
        }
    }
    std::vector<std::string> affected = clearRegion(*track, startFrame, endFrame);
    Clip clip;
    clip.id = makeUuid();
    clip.mediaRef = mediaRef;
    clip.mediaType = media->type;
    clip.sourceClipType = media->type;
    clip.startFrame = startFrame;
    clip.durationFrames = durationFrames;
    clip.trimStartFrame = trimStartFrame;
    clip.trimEndFrame = sourceDuration - trimStartFrame - durationFrames;
    track->clips.push_back(clip);
    track->sortClips();
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    affected.push_back(clip.id);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Place Clip"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::moveClip(const std::string& clipId, int64_t startFrame) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    Track* track = timeline->trackForClip(clipId);
    if (!track) return {EditStatus::Invalid, {}, "clip was not found"};
    Clip* current = track->clipById(clipId);
    if (!current || startFrame < 0) return {EditStatus::Invalid, {}, "clip or target frame is invalid"};
    if (current->startFrame == startFrame) return {EditStatus::NoOp, {clipId}, {}};
    Timeline before = *timeline;
    Clip moved = *current;
    int64_t endFrame = 0;
    if (!addChecked(startFrame, moved.durationFrames, endFrame))
        return {EditStatus::Invalid, {}, "clip range overflows"};
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(), [&](const Clip& c) { return c.id == clipId; }), track->clips.end());
    std::vector<std::string> affected = clearRegion(*track, startFrame, endFrame);
    moved.startFrame = startFrame;
    track->clips.push_back(std::move(moved));
    track->sortClips();
    std::string error;
    if (!validateTimeline(*timeline, &error)) { *timeline = std::move(before); return {EditStatus::Invalid, {}, error}; }
    affected.push_back(clipId);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Move Clip"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::splitClip(const std::string& clipId, int64_t atFrame) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    Track* track = timeline->trackForClip(clipId);
    if (!track) return {EditStatus::Invalid, {}, "clip was not found"};
    const Clip* current = track->clipById(clipId);
    if (!current) return {EditStatus::Invalid, {}, "clip was not found"};
    const auto values = splitValue(*current, atFrame);
    if (!values) return {EditStatus::NoOp, {clipId}, "split frame must be inside the clip"};

    Timeline candidate = *timeline;
    Track& candidateTrack = *candidate.trackById(track->id);
    const auto found = std::find_if(candidateTrack.clips.begin(), candidateTrack.clips.end(),
                                    [&](const Clip& clip) { return clip.id == clipId; });
    *found = values->first;
    candidateTrack.clips.push_back(values->second);
    candidateTrack.sortClips();
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};
    const std::string rightId = values->second.id;
    const Timeline before = *timeline;
    *timeline = candidate;
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Split Clip"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {clipId, rightId}, {}};
}

EditResult EditorViewModel::trimClip(const std::string& clipId, TrimEdge edge, int64_t deltaFrames) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    Track* track = timeline->trackForClip(clipId);
    if (!track) return {EditStatus::Invalid, {}, "clip was not found"};
    const Clip* original = track->clipById(clipId);
    if (!original) return {EditStatus::Invalid, {}, "clip was not found"};
    if (deltaFrames == 0) return {EditStatus::NoOp, {clipId}, {}};
    if (deltaFrames == std::numeric_limits<int64_t>::min())
        return {EditStatus::Invalid, {}, "trim frame arithmetic overflows"};

    Timeline candidate = *timeline;
    Track& candidateTrack = *candidate.trackById(track->id);
    Clip snapshot = *candidateTrack.clipById(clipId);
    int64_t newDuration = 0;
    if (!addChecked(snapshot.durationFrames, edge == TrimEdge::Left ? -deltaFrames : deltaFrames, newDuration) ||
        newDuration <= 0) return {EditStatus::Invalid, {}, "trim would make clip duration non-positive"};
    const double sourceDeltaValue = std::round(static_cast<double>(deltaFrames) * snapshot.speed);
    if (!std::isfinite(sourceDeltaValue) ||
        sourceDeltaValue <= static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        sourceDeltaValue >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return {EditStatus::Invalid, {}, "trim source-frame arithmetic overflows"};
    const int64_t sourceDelta = static_cast<int64_t>(sourceDeltaValue);
    int64_t newStart = snapshot.startFrame;
    int64_t newTrim = 0;
    int64_t newEnd = 0;
    if (edge == TrimEdge::Left) {
        if (!addChecked(snapshot.startFrame, deltaFrames, newStart) || newStart < 0 ||
            !addChecked(snapshot.trimStartFrame, sourceDelta, newTrim) || newTrim < 0)
            return {EditStatus::Invalid, {}, "trim exceeds the available clip head"};
        if (deltaFrames < 0)
            clearRegion(candidateTrack, newStart, snapshot.startFrame, {clipId});
    } else {
        if (!addChecked(snapshot.trimEndFrame, -sourceDelta, newTrim) || newTrim < 0 ||
            !addChecked(snapshot.startFrame, newDuration, newEnd))
            return {EditStatus::Invalid, {}, "trim exceeds the available clip tail"};
        if (deltaFrames > 0)
            clearRegion(candidateTrack, snapshot.endFrame(), newEnd, {clipId});
    }

    Clip* clip = candidateTrack.clipById(clipId);
    if (!clip) return {EditStatus::Invalid, {}, "trim target was removed unexpectedly"};
    clip->startFrame = newStart;
    if (edge == TrimEdge::Left) clip->trimStartFrame = newTrim;
    else clip->trimEndFrame = newTrim;
    setDuration(*clip, newDuration);
    shiftClipTimedPayloads(*clip, edge == TrimEdge::Left ? -deltaFrames : 0);
    candidateTrack.sortClips();
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};

    std::vector<std::string> affected{clipId};
    for (const Clip& beforeClip : track->clips) {
        const Clip* afterClip = candidateTrack.clipById(beforeClip.id);
        if (!afterClip || *afterClip != beforeClip) affected.push_back(beforeClip.id);
    }
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Trim Clip"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::removeClips(const std::vector<std::string>& clipIds) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    const std::unordered_set<std::string> requested(clipIds.begin(), clipIds.end());
    std::vector<std::string> affected;
    for (const Track& track : timeline->tracks)
        for (const Clip& clip : track.clips) if (requested.count(clip.id)) affected.push_back(clip.id);
    if (affected.empty()) return {EditStatus::NoOp, {}, {}};

    Timeline candidate = *timeline;
    for (Track& track : candidate.tracks) {
        track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(),
                                        [&](const Clip& clip) { return requested.count(clip.id) != 0; }),
                          track.clips.end());
    }
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, affected.size() == 1 ? "Remove Clip" : "Remove Clips"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::setClipAudio(const std::string& clipId, double volume,
                                        int64_t fadeInFrames, int64_t fadeOutFrames) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    const Clip* current = timeline->clipById(clipId);
    if (!current) return {EditStatus::Invalid, {}, "clip was not found"};
    if (!std::isfinite(volume) || volume < 0.0 || volume > 2.0 || fadeInFrames < 0 || fadeOutFrames < 0 ||
        fadeInFrames > current->durationFrames || fadeOutFrames > current->durationFrames - fadeInFrames)
        return {EditStatus::Invalid, {}, "volume must be between 0 and 2 and fades must fit the clip duration"};
    if (current->volume == volume && current->fadeInFrames == fadeInFrames && current->fadeOutFrames == fadeOutFrames)
        return {EditStatus::NoOp, {clipId}, compatibilityFor(*timeline).level == CompatibilityLevel::Warning
            ? "advanced clip fields are preserved; preview uses base volume and linear fades" : ""};
    Timeline candidate = *timeline;
    Clip* clip = candidate.clipById(clipId);
    clip->volume = volume;
    if (clip->fadeInFrames != fadeInFrames) clip->fadeInInterpolation = Interpolation::Linear;
    if (clip->fadeOutFrames != fadeOutFrames) clip->fadeOutInterpolation = Interpolation::Linear;
    clip->fadeInFrames = fadeInFrames;
    clip->fadeOutFrames = fadeOutFrames;
    const bool preservedAutomation = clip->volumeTrack.has_value()
        || (clip->fadeInFrames > 0 && clip->fadeInInterpolation != Interpolation::Linear)
        || (clip->fadeOutFrames > 0 && clip->fadeOutInterpolation != Interpolation::Linear);
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Clip Audio"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {clipId}, preservedAutomation
        ? "audio automation is preserved; preview uses base volume and linear fades" : ""};
}

EditResult EditorViewModel::setColorSettings(const ColorSettings& settings) {
    std::string error;
    if (!validateColorSettings(settings, &error)) return {EditStatus::Invalid, {}, error};
    if (!supportedColorSettings(settings))
        return {EditStatus::Unsupported, {}, "unsupported color settings version"};
    auto& current = impl_->timelines.project_.colorSettings;
    if (current == settings) return {EditStatus::NoOp, {}, {}};
    EditorViewModelTimelines::UndoEntry entry;
    entry.name = "Color Settings";
    entry.colorBefore = current;
    entry.colorAfter = settings;
    current = settings;
    impl_->timelines.undo_.push_back(std::move(entry));
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {}, {}};
}

EditResult EditorViewModel::setMediaInputColorSpace(const std::string& mediaRef, const std::string& space) {
    if (!impl_->manifest.find(mediaRef)) return {EditStatus::Invalid, {}, "mediaRef was not found"};
    ColorSettings candidate = project().colorSettings;
    if (space.empty()) candidate.inputColorSpaces.erase(mediaRef);
    else candidate.inputColorSpaces[mediaRef] = space;
    auto result = setColorSettings(candidate);
    if (result.changed()) result.affectedIds.push_back(mediaRef);
    return result;
}

EditResult EditorViewModel::setClipEffects(const std::string& clipId, std::optional<JsonValue> effects) {
    auto* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    const auto* current = timeline->clipById(clipId);
    if (!current) return {EditStatus::Invalid, {}, "clip was not found"};
    if (current->mediaType != ClipType::Video) return {EditStatus::Unsupported, {}, "video effects require a video clip"};
    if (effects && !effects->is_array()) return {EditStatus::Invalid, {}, "clip effects must be an array"};
    if (current->effects == effects) return {EditStatus::NoOp, {clipId}, {}};
    Timeline candidate = *timeline;
    candidate.clipById(clipId)->effects = std::move(effects);
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Clip Effects"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {clipId}, {}};
}

EditResult EditorViewModel::setTrackMuted(const std::string& trackId, bool muted) {
    Timeline* timeline = impl_->active();
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    const Track* current = timeline->trackById(trackId);
    if (!current) return {EditStatus::Invalid, {}, "track was not found"};
    if (current->muted == muted) return {EditStatus::NoOp, {trackId}, {}};
    Timeline candidate = *timeline;
    candidate.trackById(trackId)->muted = muted;
    std::string error;
    if (!validateTimeline(candidate, &error)) return {EditStatus::Invalid, {}, error};
    const Timeline before = *timeline;
    *timeline = std::move(candidate);
    impl_->timelines.undo_.push_back({timeline->id, before, *timeline, "Track Mute"});
    impl_->timelines.redo_.clear();
    ++impl_->revision;
    return {EditStatus::Changed, {trackId}, {}};
}

bool EditorViewModel::undo() {
    if (!impl_->timelines.undo()) return false;
    ++impl_->revision;
    return true;
}
bool EditorViewModel::redo() {
    if (!impl_->timelines.redo()) return false;
    ++impl_->revision;
    return true;
}

bool EditorViewModel::replaceDocument(ProjectFile project, MediaManifest manifest,
                                      std::string packageDir, std::string* error) {
    if (!validateProject(project, error) || !manifest.validate(error)) return false;
    impl_->timelines.replace(std::move(project));
    impl_->manifest = std::move(manifest);
    impl_->packageDir = std::move(packageDir);
    ++impl_->revision;
    return true;
}
void EditorViewModel::setProjectDir(std::string root) { impl_->packageDir = std::move(root); }

} // namespace heisenberg
