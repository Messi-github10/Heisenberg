#pragma once

#include <Models/Clip.hpp>
#include "LutLibrary.hpp"
#include <map>
#include <MultiMedia/Video/Renderer/FilterGraph/Common/GpuProgram.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanGraphDocument.hpp>
#include <string>
#include <vector>

namespace heisenberg::renderer::color { class ColorPipeline; }

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
    std::string colorSpace;
    filtergraph::VulkanGraphDocument document;
    std::vector<ParameterUpdate> parameterUpdates;
    std::string topologyKey;
};

struct ProgramUpdate {
    filtergraph::VulkanGraphNodeId nodeId = 0;
    std::shared_ptr<const filtergraph::GpuProgram> program;
};

struct CompiledGrade {
    // Stages describe effect domains; execution uses the single complete DAG.
    std::vector<GraphStage> stages;
    filtergraph::VulkanGraphDocument document;
    std::vector<ParameterUpdate> parameterUpdates;
    std::vector<ProgramUpdate> programUpdates;
    std::string topologyKey;
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
                        GradeCompileOptions options = {},
                        renderer::color::ColorPipeline* colorPipeline = nullptr,
                        const std::map<std::string, LutLibrary::Snapshot>* lutAssets = nullptr);
};

} // namespace heisenberg::grade
