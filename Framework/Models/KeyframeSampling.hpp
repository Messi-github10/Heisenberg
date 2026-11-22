#pragma once

#include <Models/Clip.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace heisenberg::keyframes {

// JSON floating point frames, including integral-valued doubles, are invalid.
inline bool readFrame(const JsonValue& value, int64_t& output) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned() &&
        value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return false;
    output = value.get<int64_t>();
    return true;
}

inline bool readFiniteDouble(const JsonValue& value, double& output) {
    if (!value.is_number()) return false;
    const double candidate = value.get<double>();
    if (!std::isfinite(candidate)) return false;
    output = candidate;
    return true;
}

inline bool readInterpolation(const std::string& name, Interpolation& output) {
    if (name == "linear") output = Interpolation::Linear;
    else if (name == "hold") output = Interpolation::Hold;
    else if (name == "smooth") output = Interpolation::Smooth;
    else return false;
    return true;
}

// Subtract before conversion: Windows long double has only double precision.
// Unsigned subtraction gives the exact positive distance across the signed range.
inline double intervalWeight(int64_t frame, int64_t left, int64_t right,
                             Interpolation interpolation) {
    if (frame <= left) return 0.0;
    if (frame >= right) return 1.0;
    if (interpolation == Interpolation::Hold) return 0.0;
    const auto offset = static_cast<uint64_t>(frame) - static_cast<uint64_t>(left);
    const auto span = static_cast<uint64_t>(right) - static_cast<uint64_t>(left);
    const double t = static_cast<double>(static_cast<long double>(offset) /
                                         static_cast<long double>(span));
    return interpolation == Interpolation::Smooth ? t * t * (3.0 - 2.0 * t) : t;
}

template<class Value>
struct Key {
    int64_t frame = 0;
    Value value{};
    Interpolation interpolation = Interpolation::Linear;
};

// readValue supplies the domain rules; unknownInterpolation supplies policy
// (appearance rejects, Grade may diagnose and use Hold). Opaque JSON is untouched.
template<class Value, class ReadValue, class UnknownInterpolation>
bool prepareTrack(const JsonValue& track, const std::string& name,
                  std::vector<Key<Value>>& output, ReadValue readValue,
                  UnknownInterpolation unknownInterpolation, std::string* error = nullptr) {
    const auto invalid = [&](const std::string& location, const std::string& reason) {
        if (error) *error = location + ": " + reason;
        return false;
    };
    if (!track.is_object() || !track.contains("keyframes") || !track["keyframes"].is_array())
        return invalid(name, "track must contain a keyframes array");
    std::vector<Key<Value>> keys;
    keys.reserve(track["keyframes"].size());
    for (size_t i = 0; i < track["keyframes"].size(); ++i) {
        const auto& json = track["keyframes"][i];
        const std::string location = name + ".keyframes[" + std::to_string(i) + "]";
        if (!json.is_object() || !json.contains("frame") || !json.contains("value"))
            return invalid(location, "keyframe must contain frame and value");
        Key<Value> key;
        if (!readFrame(json["frame"], key.frame))
            return invalid(location, "frame must be a signed 64-bit integer");
        if (!keys.empty() && key.frame <= keys.back().frame)
            return invalid(location, "frames must be strictly increasing");
        if (!readValue(json["value"], key.value))
            return invalid(location, "value has an invalid type, is not finite or is out of range");
        if (json.contains("interpolationOut")) {
            if (!json["interpolationOut"].is_string())
                return invalid(location, "interpolationOut must be a string");
            const auto name = json["interpolationOut"].get<std::string>();
            if (!readInterpolation(name, key.interpolation)) {
                if (!unknownInterpolation(name, location))
                    return invalid(location, "unsupported interpolation '" + name + "'");
                key.interpolation = Interpolation::Hold;
            }
        }
        keys.push_back(std::move(key));
    }
    output = std::move(keys);
    return true;
}

struct Interval {
    size_t left = 0;
    size_t right = 0;
    double weight = 0.0;
};

// Caller handles empty tracks with its constant fallback.
template<class Value>
Interval findInterval(const std::vector<Key<Value>>& keys, int64_t frame) {
    const auto right = std::upper_bound(keys.begin(), keys.end(), frame,
        [](int64_t frame, const Key<Value>& key) { return frame < key.frame; });
    if (right == keys.begin()) return {};
    const auto leftIndex = static_cast<size_t>(right - keys.begin() - 1);
    if (right == keys.end()) return {leftIndex, leftIndex, 0.0};
    return {leftIndex, leftIndex + 1,
            intervalWeight(frame, keys[leftIndex].frame, right->frame, keys[leftIndex].interpolation)};
}

inline double interpolate(double left, double right, double weight) {
    // std::lerp also avoids overflow for large, opposite-sign finite endpoints.
    return std::lerp(left, right, weight);
}

} // namespace heisenberg::keyframes
