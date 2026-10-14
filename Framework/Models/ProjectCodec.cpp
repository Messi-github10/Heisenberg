#include "ProjectCodec.hpp"

#include <algorithm>
#include <stdexcept>

namespace heisenberg {
namespace {
using json = nlohmann::ordered_json;
void setError(std::string* error, const std::string& message) { if (error) *error = message; }
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
