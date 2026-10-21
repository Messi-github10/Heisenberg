#pragma once

#include "ColorGrade.hpp"
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>

namespace heisenberg::grade {

enum class ColorGradeProgramDomain { LinearACEScg, ToneACEScct, Auxiliary };
struct ColorGradeGpuStage {
    std::string domain; // Exact OCIO color-space name; caller performs conversions.
    renderer::color::GpuProgram program;
};

// Throws std::invalid_argument for invalid strongly typed state. Scalar values
// only change uniformData, not shader/layout/cacheId. Curve changes change ID.
renderer::color::GpuProgram makeColorGradeGpuProgram(
    const ColorGradeState& state, ColorGradeProgramDomain domain);
// Transactional; omits identity domains, disabled effects, and external LUTs.
bool buildColorGradeGpuPrograms(const ColorGradeState& state,
    std::vector<ColorGradeGpuStage>& output, std::string* error = nullptr);

// Independent scalar math oracle for tests/analysis ONLY. Runtime rendering
// never calls this function and has no CPU pixel fallback. Input/output are in
// the explicitly supplied domain; alpha is copied unchanged.
std::array<float,4> referenceColorGradePixel(const ColorGradeState& state,
    ColorGradeProgramDomain domain, std::array<float,4> pixel);

} // namespace heisenberg::grade
