#pragma once

#include "GradeCompiler.hpp"

namespace heisenberg::grade {

// Payload is {value?, track?:{keyframes:[{frame,value,interpolationOut?}]}}.
// Missing value/empty track use fallback; outside the track holds its endpoints.
// Keys may precede frame zero after a trim. Keys must be strictly increasing.
// Numeric Linear uses the left key's interpolationOut; integer results round
// once (half away from zero). Boolean values are discrete Hold even for Linear.
// Smooth/unknown interpolation emits a diagnostic, refuses strict compilation,
// and explicitly falls back to Hold only in non-strict preview. Invalid JSON,
// non-finite values, and unrepresentable values fail even in non-strict mode.
// A null payload pointer means the parameter was absent (not JSON null).
bool sampleParameter(const JsonValue* payload,
                     filtergraph::VulkanParameterValue fallback,
                     int64_t clipLocalFrame,
                     filtergraph::VulkanParameterValue& output,
                     std::vector<GradeDiagnostic>& diagnostics,
                     const std::string& effectId,
                     const std::string& parameter,
                     std::string* error = nullptr,
                     GradeCompileOptions options = {});

} // namespace heisenberg::grade
