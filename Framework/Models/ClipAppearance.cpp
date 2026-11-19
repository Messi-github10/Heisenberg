#include <Models/ClipAppearance.hpp>

#include <cmath>

namespace heisenberg {

bool ClipAppearancePatch::empty() const {
    return !centerX && !centerY && !width && !height && !rotation &&
        !flipHorizontal && !flipVertical && !cropLeft && !cropTop &&
        !cropRight && !cropBottom && !edgeRounding && !edgeSoftness && !opacity;
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

} // namespace heisenberg
