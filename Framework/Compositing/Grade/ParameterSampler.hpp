#pragma once

#include "GradeCompiler.hpp"

namespace heisenberg::grade {

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
