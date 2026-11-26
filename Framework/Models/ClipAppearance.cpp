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

namespace {

std::optional<KeyframeTrackPayload>& appearanceTrack(Clip& clip, AppearanceProperty property) {
    switch (property) {
    case AppearanceProperty::Opacity: return clip.opacityTrack;
    case AppearanceProperty::Position: return clip.positionTrack;
    case AppearanceProperty::Scale: return clip.scaleTrack;
    case AppearanceProperty::Rotation: return clip.rotationTrack;
    case AppearanceProperty::Crop: return clip.cropTrack;
    }
    return clip.opacityTrack;
}

ClipAppearancePatch propertyPatch(const SampledClipAppearance& value, AppearanceProperty property) {
    ClipAppearancePatch patch;
    switch (property) {
    case AppearanceProperty::Opacity: patch.opacity = value.opacity; break;
    case AppearanceProperty::Position:
        patch.centerX = value.transform.centerX; patch.centerY = value.transform.centerY; break;
    case AppearanceProperty::Scale:
        patch.width = value.transform.width; patch.height = value.transform.height; break;
    case AppearanceProperty::Rotation: patch.rotation = value.transform.rotation; break;
    case AppearanceProperty::Crop:
        patch.cropLeft = value.crop.left; patch.cropTop = value.crop.top;
        patch.cropRight = value.crop.right; patch.cropBottom = value.crop.bottom; break;
    }
    return patch;
}

void applyPatch(Clip& clip, const ClipAppearancePatch& patch) {
    if (patch.centerX) clip.transform.centerX = *patch.centerX;
    if (patch.centerY) clip.transform.centerY = *patch.centerY;
    if (patch.width) clip.transform.width = *patch.width;
    if (patch.height) clip.transform.height = *patch.height;
    if (patch.rotation) clip.transform.rotation = *patch.rotation;
    if (patch.flipHorizontal) clip.transform.flipHorizontal = *patch.flipHorizontal;
    if (patch.flipVertical) clip.transform.flipVertical = *patch.flipVertical;
    if (patch.cropLeft) clip.crop.left = *patch.cropLeft;
    if (patch.cropTop) clip.crop.top = *patch.cropTop;
    if (patch.cropRight) clip.crop.right = *patch.cropRight;
    if (patch.cropBottom) clip.crop.bottom = *patch.cropBottom;
    if (patch.edgeRounding) clip.edgeRounding = *patch.edgeRounding;
    if (patch.edgeSoftness) clip.edgeSoftness = *patch.edgeSoftness;
    if (patch.opacity) clip.opacity = *patch.opacity;
    if (patch.blendMode) clip.blendMode = *patch.blendMode;
}

void writePropertyValue(JsonValue& value, const Clip& clip, AppearanceProperty property,
                        const ClipAppearancePatch* changed = nullptr) {
    switch (property) {
    case AppearanceProperty::Opacity: value = clip.opacity; break;
    case AppearanceProperty::Rotation: value = clip.transform.rotation; break;
    case AppearanceProperty::Position:
    case AppearanceProperty::Scale:
        if (!value.is_object()) value = JsonValue::object();
        if (!changed || (property == AppearanceProperty::Position ? changed->centerX.has_value() : changed->width.has_value()))
            value["a"] = property == AppearanceProperty::Position ? clip.transform.centerX : clip.transform.width;
        if (!changed || (property == AppearanceProperty::Position ? changed->centerY.has_value() : changed->height.has_value()))
            value["b"] = property == AppearanceProperty::Position ? clip.transform.centerY : clip.transform.height;
        break;
    case AppearanceProperty::Crop:
        if (!value.is_object()) value = JsonValue::object();
        if (!changed || changed->cropLeft) value["left"] = clip.crop.left;
        if (!changed || changed->cropTop) value["top"] = clip.crop.top;
        if (!changed || changed->cropRight) value["right"] = clip.crop.right;
        if (!changed || changed->cropBottom) value["bottom"] = clip.crop.bottom;
        break;
    }
}

bool validProperty(AppearanceProperty property) {
    return property >= AppearanceProperty::Opacity && property <= AppearanceProperty::Crop;
}

} // namespace

bool prepareAppearanceProperty(const Clip& clip, AppearanceProperty property,
                               PreparedAppearanceProperty& output, std::string* error) {
    if (!validProperty(property)) {
        if (error) *error = "unknown appearance property";
        return false;
    }
    // Isolate the selected property before invoking the shared numeric sampler.
    // No unrelated constants, tracks, or opaque effects enter validation.
    Clip isolated;
    switch (property) {
    case AppearanceProperty::Opacity: isolated.opacity = clip.opacity; isolated.opacityTrack = clip.opacityTrack; break;
    case AppearanceProperty::Position:
        isolated.transform.centerX = clip.transform.centerX; isolated.transform.centerY = clip.transform.centerY;
        isolated.positionTrack = clip.positionTrack; break;
    case AppearanceProperty::Scale:
        isolated.transform.width = clip.transform.width; isolated.transform.height = clip.transform.height;
        isolated.scaleTrack = clip.scaleTrack; break;
    case AppearanceProperty::Rotation:
        isolated.transform.rotation = clip.transform.rotation; isolated.rotationTrack = clip.rotationTrack; break;
    case AppearanceProperty::Crop: isolated.crop = clip.crop; isolated.cropTrack = clip.cropTrack; break;
    }
    PreparedAppearanceProperty candidate;
    candidate.property_ = property;
    if (!prepareClipAppearance(isolated, candidate.prepared_, error)) return false;
    const auto& track = appearanceTrack(isolated, property);
    if (track) for (const auto& key : track->value["keyframes"]) {
        candidate.frames_.push_back(key["frame"].get<int64_t>());
        Interpolation interpolation = Interpolation::Linear;
        if (key.contains("interpolationOut"))
            keyframes::readInterpolation(key["interpolationOut"].get<std::string>(), interpolation);
        candidate.interpolations_.push_back(interpolation);
    }
    output = std::move(candidate);
    return true;
}

bool PreparedAppearanceProperty::sample(int64_t frame, ClipAppearancePropertyState& output,
                                        std::string* error) const {
    SampledClipAppearance appearance;
    if (!prepared_.sample(frame, appearance, error)) return false;
    ClipAppearancePropertyState candidate;
    candidate.sampled = propertyPatch(appearance, property_);
    candidate.keyframes = frames_;
    candidate.animated = !frames_.empty();
    const auto next = std::upper_bound(frames_.begin(), frames_.end(), frame);
    if (next != frames_.begin()) {
        const auto index = static_cast<size_t>(next - frames_.begin() - 1);
        candidate.exactKey = frames_[index] == frame;
        candidate.interpolation = interpolations_[index];
    }
    output = std::move(candidate);
    return true;
}

bool inspectClipAppearanceProperty(const Clip& clip, AppearanceProperty property, int64_t frame,
                                   ClipAppearancePropertyState& output, std::string* error) {
    PreparedAppearanceProperty prepared;
    return prepareAppearanceProperty(clip, property, prepared, error) && prepared.sample(frame, output, error);
}

bool applyClipAppearanceEdit(const Clip& clip, const ClipAppearanceEdit& edit,
                             Clip& output, std::string* error) {
    if (error) error->clear();
    const auto invalid = [&](const char* reason) { if (error) *error = reason; return false; };
    if (edit.action != AppearanceEditAction::Values && edit.action != AppearanceEditAction::ToggleKeyframe &&
        edit.action != AppearanceEditAction::SetInterpolation) return invalid("unknown appearance edit action");
    if (edit.action == AppearanceEditAction::SetInterpolation &&
        edit.interpolation != Interpolation::Linear && edit.interpolation != Interpolation::Hold &&
        edit.interpolation != Interpolation::Smooth) return invalid("unknown appearance interpolation");
    Clip candidate = clip;
    if (!edit.property) {
        if (edit.action != AppearanceEditAction::Values)
            return invalid("keyframe actions require an appearance property");
        if (edit.patch.opacity || edit.patch.centerX || edit.patch.centerY || edit.patch.width ||
            edit.patch.height || edit.patch.rotation || edit.patch.cropLeft || edit.patch.cropTop ||
            edit.patch.cropRight || edit.patch.cropBottom)
            return invalid("animated fields require an appearance property");
        if (!validateClipAppearancePatch(clip, edit.patch, error)) return false;
        applyPatch(candidate, edit.patch);
        output = std::move(candidate);
        return true;
    }
    const auto property = *edit.property;
    ClipAppearancePropertyState state;
    if (!inspectClipAppearanceProperty(clip, property, edit.localFrame, state, error)) return false;
    if ((state.animated || edit.action != AppearanceEditAction::Values) &&
        (edit.localFrame < 0 || edit.localFrame >= clip.durationFrames))
        return invalid("appearance keyframe edit frame must be within the clip duration");
    // Enforce a single group per intent; constant extras use an absent property.
    ClipAppearancePatch outside = edit.patch;
    switch (property) {
    case AppearanceProperty::Opacity: outside.opacity.reset(); break;
    case AppearanceProperty::Position: outside.centerX.reset(); outside.centerY.reset(); break;
    case AppearanceProperty::Scale: outside.width.reset(); outside.height.reset(); break;
    case AppearanceProperty::Rotation: outside.rotation.reset(); break;
    case AppearanceProperty::Crop:
        outside.cropLeft.reset(); outside.cropTop.reset(); outside.cropRight.reset(); outside.cropBottom.reset(); break;
    }
    if (!outside.empty()) return invalid("appearance patch contains fields outside the selected property");
    if (edit.action != AppearanceEditAction::Values && !edit.patch.empty())
        return invalid("keyframe actions do not accept a values patch");

    auto& track = appearanceTrack(candidate, property);
    const auto found = std::lower_bound(state.keyframes.begin(), state.keyframes.end(), edit.localFrame);
    const size_t index = static_cast<size_t>(found - state.keyframes.begin());
    if (edit.action == AppearanceEditAction::SetInterpolation) {
        if (!state.exactKey) return invalid("interpolation requires an exact keyframe");
        if (state.interpolation != edit.interpolation)
            track->value["keyframes"][index]["interpolationOut"] = toString(edit.interpolation);
    } else if (edit.action == AppearanceEditAction::ToggleKeyframe && state.exactKey) {
        auto& keys = track->value["keyframes"];
        keys.erase(keys.begin() + static_cast<JsonValue::difference_type>(index));
    } else {
        Clip sampled;
        applyPatch(sampled, state.sampled);
        if (edit.action == AppearanceEditAction::Values) {
            if (!validateClipAppearancePatch(sampled, edit.patch, error)) return false;
            Clip changed = sampled;
            applyPatch(changed, edit.patch);
            if (changed == sampled) { output = clip; return true; }
            sampled = std::move(changed);
        }
        if (!state.animated && edit.action == AppearanceEditAction::Values) {
            applyPatch(candidate, edit.patch);
        } else {
            if (!track) track = KeyframeTrackPayload{JsonValue{{"keyframes", JsonValue::array()}}};
            auto& keys = track->value["keyframes"];
            if (!state.exactKey) {
                JsonValue key{{"frame", edit.localFrame}, {"value", nullptr},
                              {"interpolationOut", toString(state.interpolation)}};
                writePropertyValue(key["value"], sampled, property);
                keys.insert(keys.begin() + static_cast<JsonValue::difference_type>(index), std::move(key));
            } else {
                writePropertyValue(keys[index]["value"], sampled, property, &edit.patch);
            }
        }
    }
    output = std::move(candidate);
    return true;
}

} // namespace heisenberg
