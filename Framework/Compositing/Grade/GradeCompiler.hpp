#pragma once

#include <Models/Clip.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanGraphDocument.hpp>
#include <string>
#include <vector>

namespace heisenberg::grade {

// Domains describe the pixels consumed AND emitted by each graph. The caller
// uses Color processors at domain boundaries; Grade contains no ACES transform.
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
    // Values already reside in document. Also usable with setParameters() on
    // a cached executable graph whose node IDs/topology match this stage.
    std::vector<ParameterUpdate> parameterUpdates;
    // Deterministic domain + ordered node IDs/filter IDs + edges, no values.
    // Equal keys permit reuse of the GPU graph followed by setParameters().
    std::string topologyKey;
};

struct CompiledGrade {
    std::vector<GraphStage> stages;
    std::vector<GradeDiagnostic> diagnostics;
    bool neutral() const { return stages.empty(); }
};

struct GradeCompileOptions {
    // Strict refuses unknown effects/parameters, malformed payloads and Smooth.
    // Preview can explicitly opt into best effort; diagnostics are never lost.
    bool strict = true;
};

class GradeCompiler {
public:
    // Reads only clip.effects; never changes the persisted clip. Frame is in
    // clip-local timeline units, NOT source frames or trimStartFrame units.
    // Compiles ordered effects to connected, validated in-memory Vulkan graphs.
    // Neutral nodes/stages are omitted, including on an animated neutral frame.
    // On failure output retains diagnostics but contains no executable stages.
    static bool compile(const Clip& clip, int64_t clipLocalFrame,
                        CompiledGrade& output, std::string* error = nullptr,
                        GradeCompileOptions options = {});
};

} // namespace heisenberg::grade
