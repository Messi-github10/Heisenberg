#include "ParameterSampler.hpp"
#include <Models/KeyframeSampling.hpp>
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
    return keyframes::readFiniteDouble(value, result);
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

    std::vector<keyframes::Key<double>> keys;
    if (payload->contains("track")) {
        std::string unsupportedLocation;
        const auto unknownInterpolation = [&](const std::string& interpolation, const std::string& location) {
            diagnostics.push_back({"unsupported_interpolation", effectId, parameter,
                "Effect '" + effectId + "' parameter '" + parameter + "': " + location +
                ": interpolation '" + interpolation + "' has no defined Grade semantics; preview uses Hold"});
            if (unsupportedLocation.empty()) unsupportedLocation = location;
            return true; // Finish validating all keys before applying strict policy.
        };
        std::string reason;
        if (!keyframes::prepareTrack((*payload)["track"], "track", keys,
                [&](const JsonValue& value, double& result) { return representable(value, type, result); },
                unknownInterpolation, &reason)) {
            return invalid(reason);
        }
        if (!unsupportedLocation.empty() && options.strict) {
            if (error) *error = "Unsupported keyframe interpolation in effect '" + effectId +
                "' parameter '" + parameter + "': " + unsupportedLocation;
            return false;
        }
    }

    double sampled = base;
    if (!keys.empty()) {
        const auto interval = keyframes::findInterval(keys, clipLocalFrame);
        sampled = keys[interval.left].value;
        // Boolean parameters always hold, even when the segment says Linear/Smooth.
        if (type != 2)
            sampled = keyframes::interpolate(sampled, keys[interval.right].value, interval.weight);
    }

    if (!std::isfinite(sampled)) return invalid("sampled value is not finite");
    if ((type == 0 && (std::round(sampled) < std::numeric_limits<int32_t>::min() ||
                       std::round(sampled) > std::numeric_limits<int32_t>::max())) ||
        (type == 1 && std::abs(sampled) > std::numeric_limits<float>::max()))
        return invalid("sampled value is out of range");

    if (type == 0) output = static_cast<int32_t>(std::round(sampled));
    else if (type == 1) output = static_cast<float>(sampled);
    else output = sampled != 0.0;
    return true;
}

} // namespace heisenberg::grade
