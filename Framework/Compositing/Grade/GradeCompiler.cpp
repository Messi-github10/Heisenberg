#include "GradeCompiler.hpp"
#include "ParameterSampler.hpp"

#include <algorithm>
#include <initializer_list>
#include <unordered_set>

namespace heisenberg::grade {
namespace {

using namespace filtergraph;

struct Operation {
    GradeDomain domain;
    std::string filterId;
    VulkanGraphParameter parameters;
};

bool diagnostic(CompiledGrade& output, const std::string& code,
                const std::string& effectId, const std::string& parameter,
                const std::string& message, std::string* error, bool reject) {
    output.diagnostics.push_back({code, effectId, parameter, message});
    if (reject && error) *error = message;
    return !reject;
}

std::string topology(const GraphStage& stage) {
    std::string result = stage.domain == GradeDomain::ACEScg ? "ACEScg" : "ACEScct";
    for (const auto& node : stage.document.nodes()) {
        result += "/" + std::to_string(node.id) + ":" + node.filterId;
    }
    for (const auto& edge : stage.document.edges()) {
        result += "/" + std::to_string(edge.output.nodeId) + ":" + std::to_string(edge.output.pinIndex)
            + ">" + std::to_string(edge.input.nodeId) + ":" + std::to_string(edge.input.pinIndex);
    }
    return result;
}

} // namespace

bool GradeCompiler::compile(const Clip& clip, int64_t clipLocalFrame,
                            CompiledGrade& output, std::string* error,
                            GradeCompileOptions options) {
    output = {};
    if (error) error->clear();
    if (!clip.effects) return true;
    auto invalid = [&](const std::string& id, const std::string& name, const std::string& reason) {
        return diagnostic(output, "invalid_effect", id, name, reason, error, true);
    };
    if (!clip.effects->is_array()) return invalid({}, {}, "Clip effects must be an array");
    std::vector<Operation> operations;
    for (const auto& effect : *clip.effects) {
        if (!effect.is_object()) return invalid({}, {}, "Effect entry must be an object");
        std::string id;
        if (effect.contains("id")) {
            if (!effect["id"].is_string()) return invalid({}, {}, "Effect id must be a string");
            id = effect["id"].get<std::string>();
        }
        if (!effect.contains("type") || !effect["type"].is_string()) {
            return invalid(id, {}, "Effect type must be a string");
        }
        const std::string type = effect["type"].get<std::string>();
        if (effect.contains("enabled") && !effect["enabled"].is_boolean()) {
            return invalid(id, "enabled", "Effect enabled must be boolean");
        }
        if (effect.contains("params") && !effect["params"].is_object()) {
            return invalid(id, {}, "Effect params must be an object");
        }
        const bool enabled = !effect.contains("enabled") || effect["enabled"].get<bool>();
        if (!enabled) continue;
        const bool basic = type == "video.basicGrade";
        const bool exposure = type == "video.exposure" || type == "exposure";
        const bool contrast = type == "video.contrast" || type == "contrast";
        const bool saturation = type == "video.saturation" || type == "saturation";
        const bool blur = type == "video.blur" || type == "video.gaussianBlur"
            || type == "gaussianblur" || type == "gaussian_blur";
        if (!basic && !exposure && !contrast && !saturation && !blur) {
            if (!diagnostic(output, "unsupported_effect", id, {},
                            "Unsupported enabled effect '" + type + "' (" + id + ")", error, options.strict)) return false;
            continue;
        }
        const JsonValue empty = JsonValue::object();
        const auto& params = effect.contains("params") ? effect["params"] : empty;
        std::unordered_set<std::string> allowed;
        if (basic || exposure) allowed.insert("exposure");
        if (basic || contrast) allowed.insert("contrast");
        if (basic || saturation) allowed.insert("saturation");
        if (blur) allowed = {"radius", "blurRadius", "sigma"};
        for (const auto& [name, payload] : params.items()) {
            if (!allowed.contains(name)
                && !diagnostic(output, "unsupported_parameter", id, name,
                               "Unsupported parameter '" + name + "' in effect '" + id + "'", error, options.strict)) return false;
            static_cast<void>(payload);
        }
        auto sample = [&](const std::string& name, VulkanParameterValue fallback,
                          VulkanParameterValue& sampled) {
            const JsonValue* payload = params.contains(name) ? &params[name] : nullptr;
            return sampleParameter(payload, fallback, clipLocalFrame, sampled,
                                   output.diagnostics, id, name, error, options);
        };
        auto numericOperation = [&](const std::string& name, float fallback,
                                    GradeDomain domain, const std::string& filter,
                                    float minimum, float maximum) {
            VulkanParameterValue sampled;
            if (!sample(name, fallback, sampled)) return false;
            const float value = std::get<float>(sampled);
            if (value < minimum || value > maximum) {
                return invalid(id, name, "Effect '" + id + "' parameter '" + name + "' exceeds its supported range");
            }
            if (value != fallback) operations.push_back({domain, filter, {{name, value}}});
            return true;
        };
        // Explicit semantics: exposure is linear stops; contrast/saturation
        // operate in ACEScct, never run the legacy BT.2020/clamping shaders.
        if ((basic || exposure) && !numericOperation("exposure", 0.0f, GradeDomain::ACEScg,
                "grade_exposure_acescg", -32.0f, 32.0f)) return false;
        if ((basic || contrast) && !numericOperation("contrast", 1.0f, GradeDomain::ACEScct,
                "grade_contrast_acescct", 0.0f, 4.0f)) return false;
        if ((basic || saturation) && !numericOperation("saturation", 1.0f, GradeDomain::ACEScct,
                "grade_saturation_acescct", 0.0f, 2.0f)) return false;
        if (blur) {
            if (params.contains("radius") && params.contains("blurRadius")) {
                return invalid(id, "radius", "Specify radius or blurRadius, not both");
            }
            VulkanParameterValue radius, sigma;
            const std::string radiusName = params.contains("blurRadius") ? "blurRadius" : "radius";
            if (!sample(radiusName, int32_t{0}, radius) || !sample("sigma", 0.0f, sigma)) return false;
            const int32_t r = std::get<int32_t>(radius);
            const float s = std::get<float>(sigma);
            if (r < 0 || r > 32 || s < 0.0f) return invalid(id, "radius", "Gaussian radius must be 0..32 and sigma must be nonnegative");
            if (r != 0) operations.push_back({GradeDomain::ACEScg, "grade_gaussian_blur_acescg",
                                            {{"blurRadius", r}, {"sigma", s}}});
        }
    }
    // Adjacent equal domains coalesce without reordering operations. Build
    // transactionally so even a late graph error never exposes partial output.
    std::vector<GraphStage> stages;
    for (const auto& operation : operations) {
        if (stages.empty() || stages.back().domain != operation.domain) {
            stages.emplace_back();
            stages.back().domain = operation.domain;
            stages.back().document = VulkanGraphDocument::emptyStage();
        }
        auto& stage = stages.back();
        const auto previous = stage.parameterUpdates.empty() ? kVulkanGraphInputNodeId
                                                            : stage.parameterUpdates.back().nodeId;
        const auto node = stage.document.addNode(operation.filterId, operation.parameters);
        if (node == 0 || !stage.document.connect({previous, 0}, {node, 0})) {
            return diagnostic(output, "graph_construction_failed", {}, {},
                              "Cannot build Grade graph for '" + operation.filterId + "'; verify shader registry and graph endpoints", error, true);
        }
        stage.parameterUpdates.push_back({node, operation.parameters});
    }
    for (auto& stage : stages) {
        std::string graphError;
        if (!stage.document.connect({stage.parameterUpdates.back().nodeId, 0}, {kVulkanGraphOutputNodeId, 0})
            || !stage.document.validate(&graphError)) {
            return diagnostic(output, "graph_validation_failed", {}, {},
                              "Invalid Grade execution graph: " + graphError, error, true);
        }
        stage.topologyKey = topology(stage);
    }
    output.stages = std::move(stages);
    return true;
}

} // namespace heisenberg::grade
