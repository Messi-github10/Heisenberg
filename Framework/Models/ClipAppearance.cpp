#include <Models/ClipAppearance.hpp>
#include <Models/BlendMode.hpp>

#include <cmath>

namespace heisenberg {

bool ClipAppearancePatch::empty() const {
    return !centerX && !centerY && !width && !height && !rotation &&
        !flipHorizontal && !flipVertical && !cropLeft && !cropTop &&
        !cropRight && !cropBottom && !edgeRounding && !edgeSoftness && !opacity && !blendMode;
}

bool validateConstantGeometry(const Clip& clip, std::string* error) {
    const auto invalid = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const auto unit = [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    const auto& transform = clip.transform;
    if (!std::isfinite(transform.centerX) || !std::isfinite(transform.centerY) ||
        !std::isfinite(transform.rotation))
        return invalid("clip position and rotation must be finite");
    if (!std::isfinite(transform.width) || !std::isfinite(transform.height) ||
        transform.width <= 0.0 || transform.height <= 0.0)
        return invalid("clip width and height must be finite and positive");
    const auto& crop = clip.crop;
    if (!unit(crop.left) || !unit(crop.top) || !unit(crop.right) || !unit(crop.bottom) ||
        crop.left + crop.right >= 1.0 || crop.top + crop.bottom >= 1.0)
        return invalid("clip crop sides must be between 0 and 1 and opposite sides must sum to less than 1");
    if (!unit(clip.edgeRounding) || !unit(clip.edgeSoftness))
        return invalid("clip edge rounding and softness must be finite and between 0 and 1");
    return true;
}

bool validateClipAppearancePatch(const Clip& clip, const ClipAppearancePatch& patch,
                                 std::string* error) {
    const auto invalid = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const auto finite = [](const std::optional<double>& value) {
        return !value || std::isfinite(*value);
    };
    const auto positive = [&](const std::optional<double>& value) {
        return finite(value) && (!value || *value > 0.0);
    };
    const auto unit = [&](const std::optional<double>& value) {
        return finite(value) && (!value || (*value >= 0.0 && *value <= 1.0));
    };
    if (!finite(patch.centerX) || !finite(patch.centerY) || !finite(patch.rotation))
        return invalid("clip position and rotation must be finite");
    if (!positive(patch.width) || !positive(patch.height))
        return invalid("clip width and height must be finite and positive");
    if (!unit(patch.edgeRounding) || !unit(patch.edgeSoftness))
        return invalid("clip edge rounding and softness must be finite and between 0 and 1");
    if (patch.blendMode && !parseBlendMode(patch.blendMode))
        return invalid("blend mode must be a known canonical name");
    if (!unit(patch.opacity))
        return invalid("opacity must be finite and between 0 and 1");
    if (patch.cropLeft || patch.cropTop || patch.cropRight || patch.cropBottom) {
        const Crop crop{patch.cropLeft.value_or(clip.crop.left), patch.cropTop.value_or(clip.crop.top),
                        patch.cropRight.value_or(clip.crop.right), patch.cropBottom.value_or(clip.crop.bottom)};
        if (!unit(crop.left) || !unit(crop.top) || !unit(crop.right) || !unit(crop.bottom) ||
            crop.left + crop.right >= 1.0 || crop.top + crop.bottom >= 1.0)
            return invalid("clip crop sides must be between 0 and 1 and opposite sides must sum to less than 1");
    }
    return true;
}

namespace {

bool unit(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool validCrop(const Crop& crop) {
    return unit(crop.left) && unit(crop.top) && unit(crop.right) && unit(crop.bottom) &&
        crop.left + crop.right < 1.0 && crop.top + crop.bottom < 1.0;
}

bool readPair(const JsonValue& value, std::array<double, 2>& output) {
    return value.is_object() && value.contains("a") && value.contains("b") &&
        keyframes::readFiniteDouble(value["a"], output[0]) &&
        keyframes::readFiniteDouble(value["b"], output[1]);
}

bool readCrop(const JsonValue& value, std::array<double, 4>& output) {
    const char* names[] = {"left", "top", "right", "bottom"};
    if (!value.is_object()) return false;
    for (size_t i = 0; i < output.size(); ++i)
        if (!value.contains(names[i]) || !keyframes::readFiniteDouble(value[names[i]], output[i]))
            return false;
    return validCrop({output[0], output[1], output[2], output[3]});
}

double mix(double a, double b, double t) {
    return keyframes::interpolate(a, b, t);
}

template<size_t N>
std::array<double, N> mix(const std::array<double, N>& a, const std::array<double, N>& b, double t) {
    std::array<double, N> result;
    for (size_t i = 0; i < N; ++i) result[i] = mix(a[i], b[i], t);
    return result;
}

template<class Value>
Value sampleTrack(const std::vector<keyframes::Key<Value>>& keys, int64_t frame, Value fallback) {
    if (keys.empty()) return fallback;
    const auto interval = keyframes::findInterval(keys, frame);
    return mix(keys[interval.left].value, keys[interval.right].value, interval.weight);
}

template<class Value>
std::string sampleLocation(const char* name, const std::vector<keyframes::Key<Value>>& keys,
                           int64_t frame) {
    if (keys.empty()) return std::string(name) + " constant";
    const auto interval = keyframes::findInterval(keys, frame);
    return std::string(name) + ".keyframes[" + std::to_string(interval.left) + "]";
}

} // namespace

bool prepareClipAppearance(const Clip& clip, PreparedClipAppearance& output, std::string* error) {
    if (error) error->clear();
    if (!validateConstantGeometry(clip, error)) return false;
    if (!unit(clip.opacity)) {
        if (error) *error = "opacity constant must be finite and between 0 and 1";
        return false;
    }
    PreparedClipAppearance candidate;
    candidate.base_ = {clip.opacity, clip.transform, clip.crop};
    const auto rejectUnknown = [](const std::string&, const std::string&) { return false; };
    const auto prepare = [&](const auto& track, const char* name, auto& keys, auto readValue) {
        return !track || keyframes::prepareTrack(track->value, name, keys, readValue, rejectUnknown, error);
    };
    const auto opacity = [](const JsonValue& value, double& result) {
        return keyframes::readFiniteDouble(value, result) && unit(result);
    };
    const auto scale = [](const JsonValue& value, std::array<double, 2>& result) {
        return readPair(value, result) && result[0] > 0.0 && result[1] > 0.0;
    };
    if (!prepare(clip.opacityTrack, "opacityTrack", candidate.opacity_, opacity) ||
        !prepare(clip.positionTrack, "positionTrack", candidate.position_, readPair) ||
        !prepare(clip.scaleTrack, "scaleTrack", candidate.scale_, scale) ||
        !prepare(clip.rotationTrack, "rotationTrack", candidate.rotation_, keyframes::readFiniteDouble) ||
        !prepare(clip.cropTrack, "cropTrack", candidate.crop_, readCrop)) return false;
    output = std::move(candidate);
    return true;
}

bool validateClipAppearanceTracks(const Clip& clip, std::string* error) {
    PreparedClipAppearance prepared;
    return prepareClipAppearance(clip, prepared, error);
}

bool PreparedClipAppearance::sample(int64_t frame, SampledClipAppearance& output, std::string* error) const {
    if (error) error->clear();
    auto result = base_;
    result.opacity = sampleTrack(opacity_, frame, base_.opacity);
    const auto position = sampleTrack(position_, frame,
        std::array<double, 2>{base_.transform.centerX, base_.transform.centerY});
    result.transform.centerX = position[0];
    result.transform.centerY = position[1];
    const auto scale = sampleTrack(scale_, frame,
        std::array<double, 2>{base_.transform.width, base_.transform.height});
    result.transform.width = scale[0];
    result.transform.height = scale[1];
    result.transform.rotation = sampleTrack(rotation_, frame, base_.transform.rotation);
    const auto crop = sampleTrack(crop_, frame,
        std::array<double, 4>{base_.crop.left, base_.crop.top, base_.crop.right, base_.crop.bottom});
    result.crop = {crop[0], crop[1], crop[2], crop[3]};
    const auto invalid = [&](const std::string& location) {
        if (error) *error = location + ": sampled value is not finite or is out of range";
        return false;
    };
    if (!unit(result.opacity)) return invalid(sampleLocation("opacityTrack", opacity_, frame));
    if (!std::isfinite(result.transform.centerX) || !std::isfinite(result.transform.centerY))
        return invalid(sampleLocation("positionTrack", position_, frame));
    if (!std::isfinite(result.transform.width) || !std::isfinite(result.transform.height) ||
        result.transform.width <= 0.0 || result.transform.height <= 0.0)
        return invalid(sampleLocation("scaleTrack", scale_, frame));
    if (!std::isfinite(result.transform.rotation))
        return invalid(sampleLocation("rotationTrack", rotation_, frame));
    if (!validCrop(result.crop)) return invalid(sampleLocation("cropTrack", crop_, frame));
    output = result;
    return true;
}

bool sampleClipAppearance(const Clip& clip, int64_t frame, SampledClipAppearance& output, std::string* error) {
    PreparedClipAppearance prepared;
    return prepareClipAppearance(clip, prepared, error) && prepared.sample(frame, output, error);
}

bool sampleClipAppearance(const Clip& clip, int64_t frame, Clip& output, std::string* error) {
    SampledClipAppearance sampled;
    if (!sampleClipAppearance(clip, frame, sampled, error)) return false;
    Clip candidate = clip;
    candidate.opacity = sampled.opacity;
    candidate.transform = sampled.transform;
    candidate.crop = sampled.crop;
    output = std::move(candidate);
    return true;
}

} // namespace heisenberg
