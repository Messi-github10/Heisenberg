#include "Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace heisenberg {
namespace {

using json = nlohmann::ordered_json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool finite(double value) { return std::isfinite(value); }

} // namespace

const char* toString(ClipType type) {
    switch (type) {
        case ClipType::Video: return "video";
        case ClipType::Audio: return "audio";
        case ClipType::Image: return "image";
        case ClipType::Text: return "text";
        case ClipType::Lottie: return "lottie";
        case ClipType::Sequence: return "sequence";
        case ClipType::Subtitle: return "subtitle";
    }
    return "video";
}

bool clipTypeFromString(const std::string& value, ClipType& type) {
    static const std::pair<const char*, ClipType> values[] = {
        {"video", ClipType::Video}, {"audio", ClipType::Audio},
        {"image", ClipType::Image}, {"text", ClipType::Text},
        {"lottie", ClipType::Lottie}, {"sequence", ClipType::Sequence},
        {"subtitle", ClipType::Subtitle},
    };
    for (const auto& [name, candidate] : values) {
        if (value == name) { type = candidate; return true; }
    }
    return false;
}

const char* toString(Interpolation value) {
    switch (value) {
        case Interpolation::Linear: return "linear";
        case Interpolation::Hold: return "hold";
        case Interpolation::Smooth: return "smooth";
    }
    return "linear";
}

bool interpolationFromString(const std::string& value, Interpolation& result) {
    if (value == "linear") result = Interpolation::Linear;
    else if (value == "hold") result = Interpolation::Hold;
    else if (value == "smooth") result = Interpolation::Smooth;
    else return false;
    return true;
}

int64_t Clip::endFrame() const {
    if (durationFrames > 0 && startFrame > std::numeric_limits<int64_t>::max() - durationFrames)
        return std::numeric_limits<int64_t>::max();
    return startFrame + durationFrames;
}

bool Clip::contains(int64_t frame) const {
    return frame >= startFrame && frame < endFrame();
}

int64_t Clip::sourceFramesConsumed() const {
    const double value = std::round(static_cast<double>(durationFrames) * speed);
    if (!finite(value) || value > static_cast<double>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(value);
}

void Clip::clampFadesToDuration() {
    fadeInFrames = std::clamp(fadeInFrames, int64_t{0}, durationFrames);
    fadeOutFrames = std::clamp(fadeOutFrames, int64_t{0}, durationFrames - fadeInFrames);
}

namespace {
void clampTrack(std::optional<KeyframeTrackPayload>& track, int64_t duration) {
    if (!track || !track->value.is_object()) return;
    auto found = track->value.find("keyframes");
    if (found == track->value.end() || !found->is_array()) return;
    json kept = json::array();
    for (json frame : *found) {
        if (!frame.is_object() || !frame.contains("frame") || !frame["frame"].is_number_integer()) continue;
        const int64_t offset = frame["frame"].get<int64_t>();
        if (offset < 0 || offset > duration) continue;
        frame["frame"] = std::min(offset, std::max<int64_t>(0, duration - 1));
        kept.push_back(std::move(frame));
    }
    *found = std::move(kept);
    if (found->empty()) track.reset();
}
}

void Clip::clampTimedPayloadsToDuration() {
    clampTrack(opacityTrack, durationFrames);
    clampTrack(positionTrack, durationFrames);
    clampTrack(scaleTrack, durationFrames);
    clampTrack(rotationTrack, durationFrames);
    clampTrack(cropTrack, durationFrames);
    clampTrack(volumeTrack, durationFrames);
    clampFadesToDuration();
}

void Clip::rescaleWordTimings(int64_t oldDuration) {
    if (!wordTimings || oldDuration <= 0 || durationFrames <= 0) return;
    const double scale = static_cast<double>(durationFrames) / oldDuration;
    for (WordTiming& timing : *wordTimings) {
        timing.startFrame = std::clamp<int64_t>(std::llround(timing.startFrame * scale), 0, durationFrames - 1);
        timing.endFrame = std::clamp<int64_t>(std::llround(timing.endFrame * scale), timing.startFrame + 1, durationFrames);
    }
}

int64_t TimelineMarker::endFrame() const { return startFrame + durationFrames; }

int64_t Track::endFrame() const {
    int64_t result = 0;
    for (const Clip& clip : clips) result = std::max(result, clip.endFrame());
    return result;
}

const Clip* Track::clipAt(int64_t frame) const {
    for (const Clip& clip : clips) if (clip.contains(frame)) return &clip;
    return nullptr;
}

const Clip* Track::clipById(const std::string& value) const {
    for (const Clip& clip : clips) if (clip.id == value) return &clip;
    return nullptr;
}

Clip* Track::clipById(const std::string& value) {
    for (Clip& clip : clips) if (clip.id == value) return &clip;
    return nullptr;
}

void Track::sortClips() {
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) {
        return a.startFrame != b.startFrame ? a.startFrame < b.startFrame : a.id < b.id;
    });
}

TimelineSettings Timeline::settings() const {
    return TimelineSettings::fromTimelineSettings(width, height, fps);
}

bool Timeline::applySettings(const TimelineSettings& value, std::string* error) {
    if (!value.valid(error)) return false;
    if (value.frameRate().denominator() != 1) {
        setError(error, "Palmier timeline frame rate must be an integer");
        return false;
    }
    const auto representable = TimelineSettings::fromTimelineSettings(value.width(), value.height(), value.frameRate().numerator());
    if (value != representable) {
        setError(error, "Palmier timeline cannot persist custom audio, color, format or sample aspect settings");
        return false;
    }
    width = value.width();
    height = value.height();
    fps = value.frameRate().numerator();
    settingsConfigured = true;
    return true;
}

int64_t Timeline::totalFrames() const {
    int64_t result = 0;
    for (const Track& track : tracks) result = std::max(result, track.endFrame());
    return result;
}

const Clip* Timeline::clipById(const std::string& value) const {
    for (const Track& track : tracks) if (const Clip* clip = track.clipById(value)) return clip;
    return nullptr;
}

Clip* Timeline::clipById(const std::string& value) {
    for (Track& track : tracks) if (Clip* clip = track.clipById(value)) return clip;
    return nullptr;
}

Track* Timeline::trackById(const std::string& value) {
    for (Track& track : tracks) if (track.id == value) return &track;
    return nullptr;
}

const Track* Timeline::trackById(const std::string& value) const {
    for (const Track& track : tracks) if (track.id == value) return &track;
    return nullptr;
}

Track* Timeline::trackForClip(const std::string& value) {
    for (Track& track : tracks) if (track.clipById(value)) return &track;
    return nullptr;
}

const Track* Timeline::trackForClip(const std::string& value) const {
    for (const Track& track : tracks) if (track.clipById(value)) return &track;
    return nullptr;
}

std::string makeUuid() {
    std::random_device device;
    std::mt19937_64 random(device());
    uint64_t high = random(), low = random();
    high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(high >> 32) << '-'
        << std::setw(4) << static_cast<uint16_t>(high >> 16) << '-'
        << std::setw(4) << static_cast<uint16_t>(high) << '-'
        << std::setw(4) << static_cast<uint16_t>(low >> 48) << '-'
        << std::setw(12) << (low & 0x0000ffffffffffffULL);
    return out.str();
}

bool validateTimeline(const Timeline& timeline, std::string* error) {
    if (timeline.id.empty() || timeline.fps <= 0 || timeline.width <= 0 || timeline.height <= 0) {
        setError(error, "timeline identity and video settings must be valid"); return false;
    }
    std::unordered_set<std::string> ids{timeline.id};
    for (const Track& track : timeline.tracks) {
        if (track.id.empty() || !ids.insert(track.id).second || !finite(track.displayHeight)) {
            setError(error, "track ids and display height must be valid"); return false;
        }
        int64_t previousEnd = 0;
        std::vector<const Clip*> sorted;
        for (const Clip& clip : track.clips) sorted.push_back(&clip);
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->startFrame < b->startFrame; });
        for (const Clip* clip : sorted) {
            if (clip->id.empty() || !ids.insert(clip->id).second || clip->mediaRef.empty()) {
                setError(error, "clip ids must be unique and mediaRef must not be empty"); return false;
            }
            const int64_t consumed = clip->sourceFramesConsumed();
            const Transform& transform = clip->transform;
            const Crop& crop = clip->crop;
            const bool numeric = finite(clip->speed) && finite(clip->volume) && finite(clip->opacity) &&
                finite(clip->edgeRounding) && finite(clip->edgeSoftness) &&
                finite(transform.centerX) && finite(transform.centerY) && finite(transform.width) &&
                finite(transform.height) && finite(transform.rotation) && finite(transform.rotationX) &&
                finite(transform.rotationY) && finite(crop.left) && finite(crop.top) &&
                finite(crop.right) && finite(crop.bottom);
            const bool sourceOverflow = consumed == std::numeric_limits<int64_t>::max() ||
                clip->trimStartFrame > std::numeric_limits<int64_t>::max() - consumed ||
                clip->trimEndFrame > std::numeric_limits<int64_t>::max() - consumed - clip->trimStartFrame;
            if (clip->startFrame < 0 || clip->durationFrames <= 0 || clip->trimStartFrame < 0 || clip->trimEndFrame < 0 ||
                !numeric || clip->speed <= 0 || clip->endFrame() == std::numeric_limits<int64_t>::max() || sourceOverflow) {
                setError(error, "clip timing is invalid"); return false;
            }
            if (clip->startFrame < previousEnd) { setError(error, "clips on a track must not overlap"); return false; }
            previousEnd = clip->endFrame();
        }
    }
    for (const TimelineMarker& marker : timeline.markers) {
        if (marker.id.empty() || !ids.insert(marker.id).second || marker.startFrame < 0 || marker.durationFrames < 0 ||
            marker.startFrame > std::numeric_limits<int64_t>::max() - marker.durationFrames ||
            !finite(marker.color.r) || !finite(marker.color.g) || !finite(marker.color.b) || !finite(marker.color.a)) {
            setError(error, "timeline marker is invalid or has a duplicated id"); return false;
        }
    }
    return true;
}

ProjectCompatibility compatibilityFor(const Timeline& timeline) {
    ProjectCompatibility result;
    bool hasVideo = false, hasAudio = false;
    for (const Track& track : timeline.tracks) {
        if ((track.type != ClipType::Video && track.type != ClipType::Audio) ||
            (track.type == ClipType::Video && hasVideo) || (track.type == ClipType::Audio && hasAudio)) {
            result.level = CompatibilityLevel::Unsupported;
            result.messages.push_back("preview and editing support at most one video and one audio track");
            return result;
        }
        hasVideo |= track.type == ClipType::Video;
        hasAudio |= track.type == ClipType::Audio;
        for (const Clip& clip : track.clips) {
            if (clip.mediaType != track.type || clip.sourceClipType != track.type) {
                result.level = CompatibilityLevel::Unsupported;
                result.messages.push_back("video and audio clips must match their owning track type");
                return result;
            }
            if (clip.opacity != 1.0 || clip.transform != Transform{} || clip.crop != Crop{} ||
                clip.edgeRounding != 0.0 || clip.edgeSoftness != 0.0 || clip.opacityTrack ||
                clip.positionTrack || clip.scaleTrack || clip.rotationTrack || clip.cropTrack ||
                clip.volumeTrack || clip.effects || clip.blendMode ||
                (clip.fadeInFrames != 0 && clip.fadeInInterpolation != Interpolation::Linear) ||
                (clip.fadeOutFrames != 0 && clip.fadeOutInterpolation != Interpolation::Linear)) {
                result.level = CompatibilityLevel::Warning;
            }
        }
    }
    if (result.level == CompatibilityLevel::Warning)
        result.messages.push_back("advanced Palmier clip appearance and audio automation are preserved; preview uses base volume and linear fades");
    return result;
}

} // namespace heisenberg
