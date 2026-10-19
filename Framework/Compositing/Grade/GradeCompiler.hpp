#pragma once

#include <Models/Clip.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanGraphDocument.hpp>
#include <string>
#include <vector>

namespace heisenberg::grade {

enum class GradeDomain { ACEScg, ACEScct };

struct GradeDiagnostic {
    std::string code;
    std::string effectId;
    std::string parameter;
    std::string message;
};

struct ParameterUpdate {
    filtergraph::VulkanGraphNodeId nodeId = 0;
    filtergraph::VulkanGraphParameter parameters;
};

struct GraphStage {
    GradeDomain domain = GradeDomain::ACEScg;
    filtergraph::VulkanGraphDocument document;
    std::vector<ParameterUpdate> parameterUpdates;
    std::string topologyKey;
};

struct CompiledGrade {
    std::vector<GraphStage> stages;
    std::vector<GradeDiagnostic> diagnostics;

    bool neutral() const {
        return stages.empty();
    }
};

struct GradeCompileOptions {
    bool strict = true;
};

class GradeCompiler {
public:
    static bool compile(const Clip& clip, int64_t clipLocalFrame,
                        CompiledGrade& output, std::string* error = nullptr,
                        GradeCompileOptions options = {});
};

} // namespace heisenberg::grade
