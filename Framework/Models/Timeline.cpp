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

template <typename T>
T valueOr(const json& object, const char* key, T fallback) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return fallback;
    return found->get<T>();
}

template <typename T>
void putOptional(json& object, const char* key, const std::optional<T>& value) {
    if (value) object[key] = *value;
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

Timeline* ProjectFile::activeTimeline() {
    if (timelines.empty()) return nullptr;
    if (activeTimelineId) for (Timeline& timeline : timelines) if (timeline.id == *activeTimelineId) return &timeline;
    return &timelines.front();
}

const Timeline* ProjectFile::activeTimeline() const {
    if (timelines.empty()) return nullptr;
    if (activeTimelineId) for (const Timeline& timeline : timelines) if (timeline.id == *activeTimelineId) return &timeline;
    return &timelines.front();
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

bool validateProject(const ProjectFile& project, std::string* error) {
    if (project.timelines.empty()) { setError(error, "project has no timelines"); return false; }
    std::unordered_set<std::string> ids;
    for (const Timeline& timeline : project.timelines) {
        if (!ids.insert(timeline.id).second) { setError(error, "timeline ids must be unique"); return false; }
        if (!validateTimeline(timeline, error)) return false;
    }
    if (project.activeTimelineId && !ids.count(*project.activeTimelineId)) {
        setError(error, "activeTimelineId does not name a timeline"); return false;
    }
    std::unordered_set<std::string> openIds;
    if (project.openTimelineIds) for (const std::string& id : *project.openTimelineIds) {
        if (!ids.count(id) || !openIds.insert(id).second) {
            setError(error, "openTimelineIds contains an unknown or duplicated timeline"); return false;
        }
    }
    return true;
}

ProjectCompatibility compatibilityFor(const Timeline& timeline) {
    ProjectCompatibility result;
    if (timeline.tracks.empty()) return result;
    if (timeline.tracks.size() != 1 || timeline.tracks.front().type != ClipType::Video) {
        result.level = CompatibilityLevel::Unsupported;
        result.messages.push_back("preview and editing currently require exactly one video track");
        return result;
    }
    for (const Clip& clip : timeline.tracks.front().clips) {
        if (clip.mediaType != ClipType::Video || clip.sourceClipType != ClipType::Video) {
            result.level = CompatibilityLevel::Unsupported;
            result.messages.push_back("preview currently supports video clips only");
            return result;
        }
        if (clip.opacity != 1.0 || clip.volume != 1.0 || clip.fadeInFrames != 0 || clip.fadeOutFrames != 0 ||
            clip.transform != Transform{} || clip.crop != Crop{} || clip.edgeRounding != 0.0 ||
            clip.edgeSoftness != 0.0 || clip.opacityTrack || clip.positionTrack || clip.scaleTrack ||
            clip.rotationTrack || clip.cropTrack || clip.volumeTrack || clip.effects || clip.blendMode) {
            result.level = CompatibilityLevel::Warning;
        }
    }
    if (result.level == CompatibilityLevel::Warning)
        result.messages.push_back("advanced Palmier clip appearance is preserved but not rendered");
    return result;
}

namespace {
json transformJson(const Transform& value) {
    return {{"centerX", value.centerX}, {"centerY", value.centerY},
            {"width", value.width}, {"height", value.height},
            {"rotation", value.rotation}, {"rotationX", value.rotationX},
            {"rotationY", value.rotationY}, {"flipHorizontal", value.flipHorizontal},
            {"flipVertical", value.flipVertical}};
}

Transform parseTransform(const json& value) {
    Transform result;
    if (!value.is_object()) return result;
    result.width = valueOr(value, "width", 1.0);
    result.height = valueOr(value, "height", 1.0);
    if (value.contains("centerX") && !value["centerX"].is_null())
        result.centerX = value["centerX"].get<double>();
    else if (value.contains("x") && !value["x"].is_null())
        result.centerX = value["x"].get<double>() + result.width - 0.5;
    if (value.contains("centerY") && !value["centerY"].is_null())
        result.centerY = value["centerY"].get<double>();
    else if (value.contains("y") && !value["y"].is_null())
        result.centerY = value["y"].get<double>() + result.height - 0.5;
    result.rotation = valueOr(value, "rotation", 0.0);
    result.rotationX = valueOr(value, "rotationX", 0.0);
    result.rotationY = valueOr(value, "rotationY", 0.0);
    result.flipHorizontal = valueOr(value, "flipHorizontal", false);
    result.flipVertical = valueOr(value, "flipVertical", false);
    return result;
}

json cropJson(const Crop& value) {
    return {{"left", value.left}, {"top", value.top},
            {"right", value.right}, {"bottom", value.bottom}};
}

Crop parseCrop(const json& value) {
    Crop result;
    if (!value.is_object()) return result;
    result.left = valueOr(value, "left", 0.0);
    result.top = valueOr(value, "top", 0.0);
    result.right = valueOr(value, "right", 0.0);
    result.bottom = valueOr(value, "bottom", 0.0);
    return result;
}

template <typename T>
std::optional<T> optionalValue(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return std::nullopt;
    return found->get<T>();
}

std::optional<KeyframeTrackPayload> optionalTrack(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return std::nullopt;
    return KeyframeTrackPayload{*found};
}

void putTrack(json& object, const char* key, const std::optional<KeyframeTrackPayload>& track) {
    if (track) object[key] = track->value;
}

json clipJson(const Clip& clip) {
    json object;
    object["id"] = clip.id;
    object["mediaRef"] = clip.mediaRef;
    object["mediaType"] = toString(clip.mediaType);
    object["sourceClipType"] = toString(clip.sourceClipType);
    object["startFrame"] = clip.startFrame;
    object["durationFrames"] = clip.durationFrames;
    object["trimStartFrame"] = clip.trimStartFrame;
    object["trimEndFrame"] = clip.trimEndFrame;
    object["speed"] = clip.speed;
    object["volume"] = clip.volume;
    object["fadeInFrames"] = clip.fadeInFrames;
    object["fadeOutFrames"] = clip.fadeOutFrames;
    object["fadeInInterpolation"] = toString(clip.fadeInInterpolation);
    object["fadeOutInterpolation"] = toString(clip.fadeOutInterpolation);
    object["opacity"] = clip.opacity;
    object["transform"] = transformJson(clip.transform);
    object["crop"] = cropJson(clip.crop);
    object["edgeRounding"] = clip.edgeRounding;
    object["edgeSoftness"] = clip.edgeSoftness;
    putOptional(object, "linkGroupId", clip.linkGroupId);
    putOptional(object, "captionGroupId", clip.captionGroupId);
    putOptional(object, "multicamGroupId", clip.multicamGroupId);
    putOptional(object, "textContent", clip.textContent);
    putOptional(object, "textStyle", clip.textStyle);
    putOptional(object, "textAnimation", clip.textAnimation);
    if (clip.wordTimings) {
        json timings = json::array();
        for (const WordTiming& timing : *clip.wordTimings)
            timings.push_back({{"text", timing.text}, {"startFrame", timing.startFrame}, {"endFrame", timing.endFrame}});
        object["wordTimings"] = std::move(timings);
    }
    putOptional(object, "textFillMode", clip.textFillMode);
    putTrack(object, "opacityTrack", clip.opacityTrack);
    putTrack(object, "positionTrack", clip.positionTrack);
    putTrack(object, "scaleTrack", clip.scaleTrack);
    putTrack(object, "rotationTrack", clip.rotationTrack);
    putTrack(object, "cropTrack", clip.cropTrack);
    putTrack(object, "volumeTrack", clip.volumeTrack);
    putOptional(object, "effects", clip.effects);
    putOptional(object, "blendMode", clip.blendMode);
    return object;
}

Clip parseClip(const json& object) {
    Clip clip;
    clip.id = valueOr(object, "id", makeUuid());
    clip.mediaRef = object.at("mediaRef").get<std::string>();
    if (!clipTypeFromString(valueOr(object, "mediaType", std::string("video")), clip.mediaType))
        throw std::runtime_error("unknown clip mediaType");
    if (!clipTypeFromString(valueOr(object, "sourceClipType", std::string("video")), clip.sourceClipType))
        throw std::runtime_error("unknown clip sourceClipType");
    clip.startFrame = object.at("startFrame").get<int64_t>();
    clip.durationFrames = object.at("durationFrames").get<int64_t>();
    clip.trimStartFrame = valueOr<int64_t>(object, "trimStartFrame", 0);
    clip.trimEndFrame = valueOr<int64_t>(object, "trimEndFrame", 0);
    clip.speed = valueOr(object, "speed", 1.0);
    clip.volume = valueOr(object, "volume", 1.0);
    clip.fadeInFrames = valueOr<int64_t>(object, "fadeInFrames", 0);
    clip.fadeOutFrames = valueOr<int64_t>(object, "fadeOutFrames", 0);
    if (!interpolationFromString(valueOr(object, "fadeInInterpolation", std::string("linear")), clip.fadeInInterpolation) ||
        !interpolationFromString(valueOr(object, "fadeOutInterpolation", std::string("linear")), clip.fadeOutInterpolation))
        throw std::runtime_error("unknown fade interpolation");
    clip.opacity = valueOr(object, "opacity", 1.0);
    if (object.contains("transform")) clip.transform = parseTransform(object["transform"]);
    if (object.contains("crop")) clip.crop = parseCrop(object["crop"]);
    clip.edgeRounding = valueOr(object, "edgeRounding", 0.0);
    clip.edgeSoftness = valueOr(object, "edgeSoftness", 0.0);
    if (clip.edgeRounding < 0.0 || clip.edgeRounding > 1.0) clip.edgeRounding = 0.0;
    if (clip.edgeSoftness < 0.0 || clip.edgeSoftness > 1.0) clip.edgeSoftness = 0.0;
    clip.linkGroupId = optionalValue<std::string>(object, "linkGroupId");
    clip.captionGroupId = optionalValue<std::string>(object, "captionGroupId");
    clip.multicamGroupId = optionalValue<std::string>(object, "multicamGroupId");
    clip.textContent = optionalValue<std::string>(object, "textContent");
    clip.textStyle = optionalValue<JsonValue>(object, "textStyle");
    clip.textAnimation = optionalValue<JsonValue>(object, "textAnimation");
    clip.textFillMode = optionalValue<JsonValue>(object, "textFillMode");
    if (object.contains("wordTimings") && object["wordTimings"].is_array()) {
        clip.wordTimings.emplace();
        for (const json& item : object["wordTimings"])
            clip.wordTimings->push_back({item.at("text").get<std::string>(), item.at("startFrame").get<int64_t>(), item.at("endFrame").get<int64_t>()});
    }
    clip.opacityTrack = optionalTrack(object, "opacityTrack");
    clip.positionTrack = optionalTrack(object, "positionTrack");
    clip.scaleTrack = optionalTrack(object, "scaleTrack");
    clip.rotationTrack = optionalTrack(object, "rotationTrack");
    clip.cropTrack = optionalTrack(object, "cropTrack");
    clip.volumeTrack = optionalTrack(object, "volumeTrack");
    clip.effects = optionalValue<JsonValue>(object, "effects");
    clip.blendMode = optionalValue<std::string>(object, "blendMode");
    return clip;
}

json markerJson(const TimelineMarker& marker) {
    return {{"id", marker.id}, {"name", marker.name}, {"startFrame", marker.startFrame},
            {"durationFrames", marker.durationFrames},
            {"color", {{"r", marker.color.r}, {"g", marker.color.g}, {"b", marker.color.b}, {"a", marker.color.a}}},
            {"comment", marker.comment}, {"status", marker.status}};
}

TimelineMarker parseMarker(const json& object) {
    TimelineMarker marker;
    marker.id = object.at("id").get<std::string>();
    marker.name = object.at("name").get<std::string>();
    marker.startFrame = object.at("startFrame").get<int64_t>();
    marker.durationFrames = object.at("durationFrames").get<int64_t>();
    if (object.contains("color")) {
        const json& c = object["color"];
        marker.color = {valueOr(c, "r", 0.0), valueOr(c, "g", 0.478), valueOr(c, "b", 1.0), valueOr(c, "a", 1.0)};
    }
    marker.comment = valueOr(object, "comment", std::string{});
    marker.status = valueOr(object, "status", std::string("open"));
    return marker;
}

json trackJson(const Track& track) {
    json clips = json::array();
    for (const Clip& clip : track.clips) clips.push_back(clipJson(clip));
    json object = {{"id", track.id}, {"type", toString(track.type)},
                   {"muted", track.muted}, {"hidden", track.hidden},
                   {"syncLocked", track.syncLocked}, {"clips", std::move(clips)},
                   {"displayHeight", track.displayHeight}};
    putOptional(object, "name", track.name);
    return object;
}

Track parseTrack(const json& object) {
    Track track;
    track.id = valueOr(object, "id", makeUuid());
    if (!clipTypeFromString(object.at("type").get<std::string>(), track.type))
        throw std::runtime_error("unknown track type");
    track.name = optionalValue<std::string>(object, "name");
    track.muted = valueOr(object, "muted", false);
    track.hidden = valueOr(object, "hidden", false);
    track.syncLocked = valueOr(object, "syncLocked", true);
    track.displayHeight = std::clamp(valueOr(object, "displayHeight", 44.0), 32.0, 200.0);
    if (object.contains("clips")) for (const json& clip : object["clips"]) track.clips.push_back(parseClip(clip));
    return track;
}

json timelineJson(const Timeline& timeline) {
    json tracks = json::array();
    for (const Track& track : timeline.tracks) tracks.push_back(trackJson(track));
    json markers = json::array();
    for (const TimelineMarker& marker : timeline.markers) markers.push_back(markerJson(marker));
    json object = {{"id", timeline.id}, {"name", timeline.name}, {"fps", timeline.fps},
                   {"width", timeline.width}, {"height", timeline.height},
                   {"settingsConfigured", timeline.settingsConfigured},
                   {"tracks", std::move(tracks)}, {"markers", std::move(markers)}};
    putOptional(object, "folderId", timeline.folderId);
    return object;
}

Timeline parseTimeline(const json& object) {
    Timeline timeline;
    timeline.id = valueOr(object, "id", makeUuid());
    timeline.name = valueOr(object, "name", std::string("Timeline 1"));
    timeline.fps = object.at("fps").get<int>();
    timeline.width = object.at("width").get<int>();
    timeline.height = object.at("height").get<int>();
    timeline.settingsConfigured = valueOr(object, "settingsConfigured", false);
    timeline.folderId = optionalValue<std::string>(object, "folderId");
    for (const json& track : object.at("tracks")) timeline.tracks.push_back(parseTrack(track));
    if (object.contains("markers")) for (const json& marker : object["markers"]) timeline.markers.push_back(parseMarker(marker));
    return timeline;
}

json viewStateJson(const TimelineViewState& state) {
    return {{"playheadFrame", state.playheadFrame}, {"zoomScale", state.zoomScale}, {"scrollOffsetX", state.scrollOffsetX}};
}

TimelineViewState parseViewState(const json& object) {
    return {valueOr<int64_t>(object, "playheadFrame", 0), valueOr(object, "zoomScale", 4.0), valueOr(object, "scrollOffsetX", 0.0)};
}
} // namespace

std::string ProjectCodec::encode(const ProjectFile& project) {
    json timelines = json::array();
    for (const Timeline& timeline : project.timelines) timelines.push_back(timelineJson(timeline));
    json root;
    root["timelines"] = std::move(timelines);
    putOptional(root, "activeTimelineId", project.activeTimelineId);
    putOptional(root, "openTimelineIds", project.openTimelineIds);
    if (project.viewStates) {
        json states = json::object();
        for (const auto& [id, state] : *project.viewStates) states[id] = viewStateJson(state);
        root["viewStates"] = std::move(states);
    }
    if (project.speakers) {
        json values = json::array();
        for (const auto& speaker : *project.speakers)
            values.push_back({{"id", speaker.id}, {"name", speaker.name}, {"color", speaker.color}, {"centroid", speaker.centroid}});
        root["speakers"] = std::move(values);
    }
    if (project.multicamGroups) {
        json values = json::array();
        for (const auto& group : *project.multicamGroups)
            values.push_back({{"id", group.id}, {"name", group.name}, {"members", group.members}, {"masterMemberId", group.masterMemberId}});
        root["multicamGroups"] = std::move(values);
    }
    return root.dump();
}

bool ProjectCodec::decode(const std::string& text, ProjectFile& result, std::string* error) {
    try {
        const json root = json::parse(text);
        if (!root.is_object() || !root.contains("timelines") || !root["timelines"].is_array())
            throw std::runtime_error("project root must contain timelines");
        ProjectFile parsed;
        for (const json& value : root["timelines"]) parsed.timelines.push_back(parseTimeline(value));
        parsed.activeTimelineId = optionalValue<std::string>(root, "activeTimelineId");
        parsed.openTimelineIds = optionalValue<std::vector<std::string>>(root, "openTimelineIds");
        if (root.contains("viewStates") && root["viewStates"].is_object()) {
            parsed.viewStates.emplace();
            for (auto it = root["viewStates"].begin(); it != root["viewStates"].end(); ++it)
                parsed.viewStates->emplace(it.key(), parseViewState(it.value()));
        }
        if (root.contains("speakers") && root["speakers"].is_array()) {
            parsed.speakers.emplace();
            for (const json& value : root["speakers"])
                parsed.speakers->push_back({value.at("id").get<int>(), value.at("name").get<std::string>(),
                                            value.at("color").get<std::vector<double>>(), value.at("centroid").get<std::vector<float>>()});
        }
        if (root.contains("multicamGroups") && root["multicamGroups"].is_array()) {
            parsed.multicamGroups.emplace();
            for (const json& value : root["multicamGroups"])
                parsed.multicamGroups->push_back({valueOr(value, "id", makeUuid()), valueOr(value, "name", std::string{}),
                                                  valueOr(value, "members", json::array()), valueOr(value, "masterMemberId", std::string{})});
        }
        if (!validateProject(parsed, error)) return false;
        result = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("Invalid Palmier project JSON: ") + exception.what());
        return false;
    }
}

} // namespace heisenberg
