#include "ParameterSampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace heisenberg::grade {
namespace {

bool fail(std::vector<GradeDiagnostic>& diagnostics,
          const std::string& effectId, const std::string& parameter,
          const std::string& message, std::string* error) {
    diagnostics.push_back({"invalid_parameter", effectId, parameter, message});
    if (error) *error = message;
    return false;
}

bool number(const JsonValue& value, double& result) {
    if (!value.is_number()) return false;
    result = value.get<double>();
    return std::isfinite(result);
}

bool representable(const JsonValue& value, size_t type, double& result) {
    if (type == 2) {
        if (!value.is_boolean()) return false;
        result = value.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (!number(value, result)) return false;
    if (type == 0) {
        const double rounded = std::round(result);
        return rounded >= std::numeric_limits<int32_t>::min()
            && rounded <= std::numeric_limits<int32_t>::max();
    }
    return std::abs(result) <= std::numeric_limits<float>::max();
}

struct Key {
    int64_t frame;
    double value;
    bool linear;
};

} // namespace

bool sampleParameter(const JsonValue* payload,
                     filtergraph::VulkanParameterValue fallback,
                     int64_t clipLocalFrame,
                     filtergraph::VulkanParameterValue& output,
                     std::vector<GradeDiagnostic>& diagnostics,
                     const std::string& effectId,
                     const std::string& parameter,
                     std::string* error,
                     GradeCompileOptions options) {
    output = fallback;
    if (error) error->clear();
    if (!payload) return true;
    auto invalid = [&](const std::string& reason) {
        return fail(diagnostics, effectId, parameter,
                    "Effect '" + effectId + "' parameter '" + parameter + "': " + reason, error);
    };
    if (!payload->is_object()) return invalid("expected a parameter object");
    const size_t type = fallback.index();
    double base = std::visit([](auto value) { return static_cast<double>(value); }, fallback);
    if (!std::isfinite(base)) return invalid("default is not finite");
    if (payload->contains("value") && !representable((*payload)["value"], type, base)) {
        return invalid("value has an invalid type, is not finite or is out of range");
    }
    std::vector<Key> keys;
    if (payload->contains("track")) {
        const auto& track = (*payload)["track"];
        if (!track.is_object() || !track.contains("keyframes") || !track["keyframes"].is_array()) {
            return invalid("track must contain a keyframes array");
        }
        bool unsupported = false;
        for (const auto& key : track["keyframes"]) {
            if (!key.is_object() || !key.contains("frame") || !key.contains("value")) {
                return invalid("keyframe must contain frame and value");
            }
            const auto& frame = key["frame"];
            if (!frame.is_number_integer()
                || (frame.is_number_unsigned() && frame.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
                return invalid("keyframe frame must be a signed 64-bit integer");
            }
            const int64_t keyFrame = frame.get<int64_t>();
            if (!keys.empty() && keyFrame <= keys.back().frame) {
                return invalid("keyframe frames must be strictly increasing");
            }
            double value = 0;
            if (!representable(key["value"], type, value)) {
                return invalid("keyframe value has an invalid type, is not finite or is out of range");
            }
            std::string interpolation = "linear";
            if (key.contains("interpolationOut")) {
                if (!key["interpolationOut"].is_string()) return invalid("interpolationOut must be a string");
                interpolation = key["interpolationOut"].get<std::string>();
            }
            const bool linear = interpolation == "linear";
            if (!linear && interpolation != "hold") {
                diagnostics.push_back({"unsupported_interpolation", effectId, parameter,
                    "Effect '" + effectId + "' parameter '" + parameter + "': interpolation '"
                    + interpolation + "' has no defined Grade semantics; preview uses Hold"});
                unsupported = true;
            }
            keys.push_back({keyFrame, value, linear});
        }
        if (unsupported && options.strict) {
            if (error) *error = "Unsupported keyframe interpolation in effect '" + effectId + "' parameter '" + parameter + "'";
            return false;
        }
    }
    double sampled = base;
    if (!keys.empty()) {
        const auto right = std::upper_bound(keys.begin(), keys.end(), clipLocalFrame,
            [](int64_t frame, const Key& key) { return frame < key.frame; });
        if (right == keys.begin()) sampled = keys.front().value;
        else {
            const auto& left = *(right - 1);
            sampled = left.value;
            if (right != keys.end() && left.linear && type != 2) {
                // Cast before subtraction to avoid overflow across signed frame limits.
                const long double offset = static_cast<long double>(clipLocalFrame) - left.frame;
                const long double span = static_cast<long double>(right->frame) - left.frame;
                const double t = static_cast<double>(offset / span);
                sampled = left.value * (1.0 - t) + right->value * t;
            }
        }
    }
    if (!std::isfinite(sampled)) return invalid("sampled value is not finite");
    if (type == 0) output = static_cast<int32_t>(std::round(sampled));
    else if (type == 1) output = static_cast<float>(sampled);
    else output = sampled != 0.0;
    return true;
}

} // namespace heisenberg::grade
