#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heisenberg {

using JsonValue = nlohmann::ordered_json;

enum class ClipType { Video, Audio, Image, Text, Lottie, Sequence, Subtitle };
enum class Interpolation { Linear, Hold, Smooth };

struct Transform {
    double centerX = 0.5;
    double centerY = 0.5;
    double width = 1.0;
    double height = 1.0;
    double rotation = 0.0;
    double rotationX = 0.0;
    double rotationY = 0.0;
    bool flipHorizontal = false;
    bool flipVertical = false;
    bool operator==(const Transform&) const = default;
};

struct Crop {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    bool operator==(const Crop&) const = default;
};

struct WordTiming {
    std::string text;
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    bool operator==(const WordTiming&) const = default;
};

struct KeyframeTrackPayload {
    JsonValue value = JsonValue::object();
    bool operator==(const KeyframeTrackPayload&) const = default;
};

struct Clip {
    std::string id;
    std::string mediaRef;
    ClipType mediaType = ClipType::Video;
    ClipType sourceClipType = ClipType::Video;
    int64_t startFrame = 0;
    int64_t durationFrames = 1;
    int64_t trimStartFrame = 0;
    int64_t trimEndFrame = 0;
    double speed = 1.0;
    double volume = 1.0;
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;
    Interpolation fadeInInterpolation = Interpolation::Linear;
    Interpolation fadeOutInterpolation = Interpolation::Linear;
    double opacity = 1.0;
    Transform transform;
    Crop crop;
    double edgeRounding = 0.0;
    double edgeSoftness = 0.0;
    std::optional<std::string> linkGroupId;
    std::optional<std::string> captionGroupId;
    std::optional<std::string> multicamGroupId;
    std::optional<std::string> textContent;
    std::optional<JsonValue> textStyle;
    std::optional<JsonValue> textAnimation;
    std::optional<std::vector<WordTiming>> wordTimings;
    std::optional<JsonValue> textFillMode;
    std::optional<KeyframeTrackPayload> opacityTrack;
    std::optional<KeyframeTrackPayload> positionTrack;
    std::optional<KeyframeTrackPayload> scaleTrack;
    std::optional<KeyframeTrackPayload> rotationTrack;
    std::optional<KeyframeTrackPayload> cropTrack;
    std::optional<KeyframeTrackPayload> volumeTrack;
    std::optional<JsonValue> effects;
    std::optional<std::string> blendMode;

    int64_t endFrame() const;
    bool contains(int64_t timelineFrame) const;
    int64_t sourceFramesConsumed() const;
    void clampFadesToDuration();
    void clampTimedPayloadsToDuration();
    void rescaleWordTimings(int64_t oldDuration);
    bool operator==(const Clip&) const = default;
};

const char* toString(ClipType type);
bool clipTypeFromString(const std::string& value, ClipType& type);
const char* toString(Interpolation interpolation);
bool interpolationFromString(const std::string& value, Interpolation& interpolation);

} // namespace heisenberg
