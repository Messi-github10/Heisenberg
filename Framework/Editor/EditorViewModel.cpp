#include "EditorViewModel.hpp"

#include <Preview/Producer.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace heisenberg {
namespace {

void setError(std::string* error, const std::string& message) { if (error) *error = message; }

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.c_str()), value.size()};
}

bool readText(const std::filesystem::path& path, std::string& text, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { setError(error, "Failed to open '" + utf8(path) + "'"); return false; }
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

bool writeAtomic(const std::filesystem::path& path, const std::string& text, std::string* error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp-" + makeUuid());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) { setError(error, "Failed to write '" + utf8(path) + "'"); return false; }
        stream << text;
        if (!stream) { setError(error, "Failed to write '" + utf8(path) + "'"); return false; }
    }
    std::filesystem::remove(path, code);
    code.clear();
    std::filesystem::rename(temporary, path, code);
    if (code) { std::filesystem::remove(temporary, code); setError(error, "Failed to replace '" + utf8(path) + "'"); return false; }
    return true;
}

bool addChecked(int64_t left, int64_t right, int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) return false;
    result = left + right;
    return true;
}

} // namespace

struct EditorViewModel::Impl {
    struct UndoEntry { std::string timelineId; Timeline before; Timeline after; std::string name; };
    bool hardwareDecode = false;
    std::string packageDir;
    ProjectFile project;
    MediaManifest manifest;
    std::vector<UndoEntry> undo;
    std::vector<UndoEntry> redo;

    Timeline* active() { return project.activeTimeline(); }
    const Timeline* active() const { return project.activeTimeline(); }
    Track* track() { Timeline* t = active(); return t && t->tracks.size() == 1 ? &t->tracks.front() : nullptr; }
    const Track* track() const { const Timeline* t = active(); return t && t->tracks.size() == 1 ? &t->tracks.front() : nullptr; }
};

namespace {
void shiftJsonKeyframeTracks(JsonValue& value, int64_t shift, int64_t duration);

std::pair<std::optional<KeyframeTrackPayload>, std::optional<KeyframeTrackPayload>>
splitTrackPayload(const std::optional<KeyframeTrackPayload>& source, int64_t offset) {
    if (!source || !source->value.is_object() || !source->value.contains("keyframes") ||
        !source->value["keyframes"].is_array()) return {source, source};
    JsonValue left = source->value, right = source->value;
    left["keyframes"] = JsonValue::array();
    right["keyframes"] = JsonValue::array();
    for (JsonValue keyframe : source->value["keyframes"]) {
        if (!keyframe.is_object() || !keyframe.contains("frame")) continue;
        const int64_t frame = keyframe["frame"].get<int64_t>();
        if (frame <= offset) left["keyframes"].push_back(keyframe);
        if (frame >= offset) {
            keyframe["frame"] = frame - offset;
            right["keyframes"].push_back(std::move(keyframe));
        }
    }
    std::optional<KeyframeTrackPayload> l, r;
    if (!left["keyframes"].empty()) l = KeyframeTrackPayload{std::move(left)};
    if (!right["keyframes"].empty()) r = KeyframeTrackPayload{std::move(right)};
    return {std::move(l), std::move(r)};
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
            if (!addChecked(keyframe["frame"].get<int64_t>(), shift, frame) || frame < 0 || frame >= duration) continue;
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
    JsonValue kept = JsonValue::array();
    for (JsonValue keyframe : payload->value["keyframes"]) {
        if (!keyframe.is_object() || !keyframe.contains("frame") || !keyframe["frame"].is_number_integer()) continue;
        int64_t frame = 0;
        if (!addChecked(keyframe["frame"].get<int64_t>(), shift, frame) || frame < 0 || frame >= duration) continue;
        keyframe["frame"] = frame;
        kept.push_back(std::move(keyframe));
    }
    payload->value["keyframes"] = std::move(kept);
    if (payload->value["keyframes"].empty()) payload.reset();
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

EditorViewModel::EditorViewModel() : impl_(std::make_unique<Impl>()) {
    Timeline timeline;
    timeline.id = makeUuid();
    impl_->project.timelines.push_back(timeline);
    impl_->project.activeTimelineId = timeline.id;
    impl_->project.openTimelineIds = std::vector<std::string>{timeline.id};
    impl_->project.viewStates = std::unordered_map<std::string, TimelineViewState>{};
}

EditorViewModel::~EditorViewModel() = default;
void EditorViewModel::setHardwareDecode(bool enabled) { impl_->hardwareDecode = enabled; }

void EditorViewModel::setProfile(Profile profile) {
    Timeline* timeline = impl_->active();
    if (!timeline) return;
    timeline->width = profile.width();
    timeline->height = profile.height();
    timeline->fps = std::max(1, static_cast<int>(std::llround(profile.fps())));
    timeline->settingsConfigured = true;
}

bool EditorViewModel::importMedia(const std::string& path, std::string* error) {
    const Timeline* timeline = impl_->active();
    if (!timeline || path.empty()) { setError(error, "media path and active timeline are required"); return false; }
    std::error_code code;
    if (!std::filesystem::exists(std::filesystem::u8path(path), code)) {
        setError(error, "media file was not found"); return false;
    }
    Profile profile = Profile::fromTimelineSettings(timeline->width, timeline->height, timeline->fps);
    Producer producer(profile);
    producer.setHardwareDecode(impl_->hardwareDecode);
    if (!producer.open(path, error)) return false;
    MediaManifestEntry entry;
    entry.id = makeUuid();
    entry.name = utf8(std::filesystem::u8path(path).stem());
    entry.type = ClipType::Video;
    entry.source = {MediaSource::Kind::External, path};
    entry.duration = static_cast<double>(producer.length()) / timeline->fps;
    entry.sourceFPS = static_cast<double>(timeline->fps);
    return impl_->manifest.add(std::move(entry), error);
}

const ProjectFile& EditorViewModel::project() const { return impl_->project; }
const MediaManifest& EditorViewModel::manifest() const { return impl_->manifest; }
MediaResolver EditorViewModel::resolver() const { return MediaResolver(impl_->manifest, impl_->packageDir); }
const Timeline* EditorViewModel::activeTimeline() const { return impl_->active(); }
Timeline* EditorViewModel::activeTimeline() { return impl_->active(); }
const Track* EditorViewModel::activeTrack() const { return impl_->track(); }
Track* EditorViewModel::activeTrack() { return impl_->track(); }
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
                                      int64_t durationFrames, int64_t trimStartFrame) {
    Timeline* timeline = impl_->active();
    const MediaManifestEntry* media = impl_->manifest.find(mediaRef);
    if (!timeline || !media) return {EditStatus::Invalid, {}, "mediaRef was not found"};
    if (media->type != ClipType::Video) return {EditStatus::Unsupported, {}, "only video media can be placed"};
    if (compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline is not a single video track"};
    if (startFrame < 0 || trimStartFrame < 0) return {EditStatus::Invalid, {}, "frame values must be non-negative"};
    const double sourceFrames = media->duration * timeline->fps;
    if (!std::isfinite(sourceFrames) || sourceFrames < 0 ||
        sourceFrames > static_cast<double>(std::numeric_limits<int64_t>::max()))
        return {EditStatus::Invalid, {}, "media duration cannot be represented in timeline frames"};
    const int64_t sourceDuration = std::llround(sourceFrames);
    if (durationFrames < 0) durationFrames = sourceDuration - trimStartFrame;
    if (durationFrames <= 0) return {EditStatus::Invalid, {}, "clip duration must be positive"};
    if (trimStartFrame > sourceDuration || durationFrames > sourceDuration - trimStartFrame)
        return {EditStatus::Invalid, {}, "clip range exceeds the source duration"};

    int64_t endFrame = 0;
    if (!addChecked(startFrame, durationFrames, endFrame))
        return {EditStatus::Invalid, {}, "clip range overflows"};
    Timeline before = *timeline;
    if (timeline->tracks.empty()) {
        Track track; track.id = makeUuid(); track.type = ClipType::Video;
        timeline->tracks.push_back(std::move(track));
    }
    Track& track = timeline->tracks.front();
    std::vector<std::string> affected = clearRegion(track, startFrame, endFrame);
    Clip clip;
    clip.id = makeUuid();
    clip.mediaRef = mediaRef;
    clip.mediaType = ClipType::Video;
    clip.sourceClipType = ClipType::Video;
    clip.startFrame = startFrame;
    clip.durationFrames = durationFrames;
    clip.trimStartFrame = trimStartFrame;
    clip.trimEndFrame = sourceDuration - trimStartFrame - durationFrames;
    track.clips.push_back(clip);
    track.sortClips();
    std::string error;
    if (!validateTimeline(*timeline, &error)) { *timeline = std::move(before); return {EditStatus::Invalid, {}, error}; }
    affected.push_back(clip.id);
    impl_->undo.push_back({timeline->id, before, *timeline, "Place Clip"});
    impl_->redo.clear();
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::moveClip(const std::string& clipId, int64_t startFrame) {
    Timeline* timeline = impl_->active();
    Track* track = impl_->track();
    if (!timeline || !track || track->type != ClipType::Video)
        return {EditStatus::Unsupported, {}, "active timeline is not a single video track"};
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
    impl_->undo.push_back({timeline->id, before, *timeline, "Move Clip"});
    impl_->redo.clear();
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::splitClip(const std::string& clipId, int64_t atFrame) {
    Timeline* timeline = impl_->active();
    Track* track = impl_->track();
    if (!timeline || !track || track->type != ClipType::Video)
        return {EditStatus::Unsupported, {}, "active timeline is not a single video track"};
    const Clip* current = track->clipById(clipId);
    if (!current) return {EditStatus::Invalid, {}, "clip was not found"};
    const auto values = splitValue(*current, atFrame);
    if (!values) return {EditStatus::NoOp, {clipId}, "split frame must be inside the clip"};

    Timeline candidate = *timeline;
    Track& candidateTrack = candidate.tracks.front();
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
    impl_->undo.push_back({timeline->id, before, *timeline, "Split Clip"});
    impl_->redo.clear();
    return {EditStatus::Changed, {clipId, rightId}, {}};
}

EditResult EditorViewModel::trimClip(const std::string& clipId, TrimEdge edge, int64_t deltaFrames) {
    Timeline* timeline = impl_->active();
    Track* track = impl_->track();
    if (!timeline || !track || track->type != ClipType::Video)
        return {EditStatus::Unsupported, {}, "active timeline is not a single video track"};
    const Clip* original = track->clipById(clipId);
    if (!original) return {EditStatus::Invalid, {}, "clip was not found"};
    if (deltaFrames == 0) return {EditStatus::NoOp, {clipId}, {}};

    Timeline candidate = *timeline;
    Track& candidateTrack = candidate.tracks.front();
    Clip snapshot = *candidateTrack.clipById(clipId);
    int64_t newDuration = 0;
    if (!addChecked(snapshot.durationFrames, edge == TrimEdge::Left ? -deltaFrames : deltaFrames, newDuration) ||
        newDuration <= 0) return {EditStatus::Invalid, {}, "trim would make clip duration non-positive"};
    const double sourceDeltaValue = std::round(static_cast<double>(deltaFrames) * snapshot.speed);
    if (!std::isfinite(sourceDeltaValue) ||
        sourceDeltaValue < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        sourceDeltaValue > static_cast<double>(std::numeric_limits<int64_t>::max()))
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
    impl_->undo.push_back({timeline->id, before, *timeline, "Trim Clip"});
    impl_->redo.clear();
    return {EditStatus::Changed, std::move(affected), {}};
}

EditResult EditorViewModel::removeClips(const std::vector<std::string>& clipIds) {
    Timeline* timeline = impl_->active();
    Track* track = impl_->track();
    if (!timeline || !track || track->type != ClipType::Video)
        return {EditStatus::Unsupported, {}, "active timeline is not a single video track"};
    const std::unordered_set<std::string> requested(clipIds.begin(), clipIds.end());
    std::vector<std::string> affected;
    for (const Clip& clip : track->clips) if (requested.count(clip.id)) affected.push_back(clip.id);
    if (affected.empty()) return {EditStatus::NoOp, {}, {}};

    const Timeline before = *timeline;
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(),
                                     [&](const Clip& clip) { return requested.count(clip.id) != 0; }),
                       track->clips.end());
    std::string error;
    if (!validateTimeline(*timeline, &error)) { *timeline = before; return {EditStatus::Invalid, {}, error}; }
    impl_->undo.push_back({timeline->id, before, *timeline, affected.size() == 1 ? "Remove Clip" : "Remove Clips"});
    impl_->redo.clear();
    return {EditStatus::Changed, std::move(affected), {}};
}

bool EditorViewModel::undo() {
    if (impl_->undo.empty()) return false;
    Impl::UndoEntry entry = std::move(impl_->undo.back());
    impl_->undo.pop_back();
    auto found = std::find_if(impl_->project.timelines.begin(), impl_->project.timelines.end(),
                              [&](const Timeline& timeline) { return timeline.id == entry.timelineId; });
    if (found == impl_->project.timelines.end()) return false;
    *found = entry.before;
    impl_->project.activeTimelineId = entry.timelineId;
    impl_->redo.push_back(std::move(entry));
    return true;
}

bool EditorViewModel::redo() {
    if (impl_->redo.empty()) return false;
    Impl::UndoEntry entry = std::move(impl_->redo.back());
    impl_->redo.pop_back();
    auto found = std::find_if(impl_->project.timelines.begin(), impl_->project.timelines.end(),
                              [&](const Timeline& timeline) { return timeline.id == entry.timelineId; });
    if (found == impl_->project.timelines.end()) return false;
    *found = entry.after;
    impl_->project.activeTimelineId = entry.timelineId;
    impl_->undo.push_back(std::move(entry));
    return true;
}

bool EditorViewModel::loadPackage(const std::string& packageDir, std::string* error) {
    std::filesystem::path root = std::filesystem::u8path(packageDir);
    if (root.filename() == "project.json") root = root.parent_path();
    std::error_code code;
    if (!std::filesystem::is_directory(root, code)) {
        setError(error, "Palmier package directory was not found");
        return false;
    }
    std::string projectText, mediaText;
    if (!readText(root / "project.json", projectText, error) ||
        !readText(root / "media.json", mediaText, error)) return false;
    ProjectFile project;
    MediaManifest manifest;
    if (!ProjectCodec::decode(projectText, project, error) ||
        !manifest.loadFromJson(mediaText, error)) return false;

    impl_->project = std::move(project);
    impl_->manifest = std::move(manifest);
    impl_->packageDir = utf8(root.lexically_normal());
    impl_->undo.clear();
    impl_->redo.clear();
    return true;
}

bool EditorViewModel::savePackage(const std::string& packageDir, std::string* error) {
    std::filesystem::path destination = std::filesystem::u8path(packageDir);
    if (destination.filename() == "project.json") destination = destination.parent_path();
    if (destination.empty()) { setError(error, "package directory is required"); return false; }
    std::string validationError;
    if (!validateProject(impl_->project, &validationError)) {
        setError(error, validationError); return false;
    }
    if (!impl_->manifest.validate(&validationError)) {
        setError(error, validationError); return false;
    }

    std::error_code code;
    std::filesystem::path source;
    if (!impl_->packageDir.empty()) {
        source = std::filesystem::absolute(std::filesystem::u8path(impl_->packageDir), code).lexically_normal();
        if (code) { setError(error, "failed to resolve source package path"); return false; }
    }
    destination = std::filesystem::absolute(destination, code).lexically_normal();
    if (code) { setError(error, "failed to resolve package path"); return false; }
    const bool saveAs = !impl_->packageDir.empty() && source != destination;
    if (saveAs && std::filesystem::exists(source, code)) {
        auto sourceIt = source.begin(), destinationIt = destination.begin();
        while (sourceIt != source.end() && destinationIt != destination.end() && *sourceIt == *destinationIt) {
            ++sourceIt; ++destinationIt;
        }
        if (sourceIt == source.end()) {
            setError(error, "Save As destination cannot be inside the source package");
            return false;
        }
        std::filesystem::create_directories(destination, code);
        if (code) { setError(error, "failed to create Save As package"); return false; }
        for (std::filesystem::recursive_directory_iterator it(source, code), end; !code && it != end; it.increment(code)) {
            const auto relative = std::filesystem::relative(it->path(), source, code);
            if (code) break;
            const auto target = destination / relative;
            if (it->is_directory()) std::filesystem::create_directories(target, code);
            else std::filesystem::copy(it->path(), target,
                                       std::filesystem::copy_options::overwrite_existing |
                                       std::filesystem::copy_options::copy_symlinks, code);
            if (code) break;
        }
        if (code) { setError(error, "failed to preserve existing package contents: " + code.message()); return false; }
    }

    std::filesystem::create_directories(destination / "media", code);
    if (code) { setError(error, "failed to create Palmier package directories"); return false; }
    if (!writeAtomic(destination / "project.json", ProjectCodec::encode(impl_->project), error) ||
        !writeAtomic(destination / "media.json", impl_->manifest.toJson(), error)) return false;
    impl_->packageDir = utf8(destination);
    return true;
}

} // namespace heisenberg
