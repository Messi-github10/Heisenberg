#include "GradeCompiler.hpp"
#include "ParameterSampler.hpp"
#include "ColorGrade.hpp"
#include "GradeGpuPrograms.hpp"
#include "LutLibrary.hpp"
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
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
    std::string colorSpace;
    std::shared_ptr<const GpuProgram> program;
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

GraphImageContract contractFor(const std::string& space) {
    auto contract = kWorkingImageContract;
    if (space == renderer::color::kGradingSpace) contract.transfer = filtergraph::ColorTransfer::acescct;
    else if (space == kColorGradeAuxiliarySpace) {
        contract.primaries = filtergraph::ColorPrimaries::bt709;
        contract.transfer = filtergraph::ColorTransfer::srgb;
    } else if (space == renderer::color::kExportDisplay) {
        contract.primaries = filtergraph::ColorPrimaries::bt709;
        contract.transfer = filtergraph::ColorTransfer::bt1886;
    } else if (space != renderer::color::kWorkingSpace) {
        throw std::invalid_argument("Unsupported Grade image domain: " + space);
    }
    return contract;
}

std::shared_ptr<const GpuProgram> lutProgram(renderer::color::ColorPipeline& pipeline,
    const LutLibrary::Snapshot& asset, float intensity) {
    thread_local std::map<std::string, std::shared_ptr<const GpuProgram>> programs;
    auto found = programs.find(asset->id);
    if (found == programs.end()) {
        auto program = pipeline.lutProcessor(asset->metadata.size, asset->samples,
            asset->metadata.domainMin, asset->metadata.domainMax).gpuProgram();
        if (!program.uniformData.empty())
            throw std::runtime_error("Static creative LUT unexpectedly requires dynamic uniforms");
        const auto function = program.functionName;
        program.shaderText += "\nlayout(std140, set=1, binding=0) uniform LutMixParams { vec4 amount; } lutMix;\n"
            "vec4 heisenbergCreativeLut(vec4 inputColor) { vec4 mapped = " + function
            + "(inputColor); return vec4(mix(inputColor.rgb, mapped.rgb, lutMix.amount.x), inputColor.a); }\n";
        program.functionName = "heisenbergCreativeLut";
        program.cacheId = "creative-tetrahedral-v1/" + asset->id + "/" + program.cacheId;
        program.uniformData.resize(16);
        program.sharedLuts = std::make_shared<const std::vector<GpuLut>>(std::move(program.luts));
        if (programs.size() >= renderer::color::kProgramCacheCapacity) programs.erase(programs.begin());
        found = programs.emplace(asset->id, std::make_shared<const GpuProgram>(std::move(program))).first;
    }
    auto program = std::make_shared<GpuProgram>(*found->second);
    std::memcpy(program->uniformData.data(), &intensity, sizeof(intensity));
    return program;
}

} // namespace

bool GradeCompiler::compile(const Clip& clip, int64_t clipLocalFrame,
                            CompiledGrade& output, std::string* error,
                            GradeCompileOptions options,
                            renderer::color::ColorPipeline* suppliedPipeline,
                            const std::map<std::string, LutLibrary::Snapshot>* lutAssets) {
    output = {};
    if (error) error->clear();
    if (!clip.effects) return true;

    auto invalid = [&](const std::string& id, const std::string& name, const std::string& reason) {
        return diagnostic(output, "invalid_effect", id, name, reason, error, true);
    };
    if (!clip.effects->is_array()) return invalid({}, {}, "Clip effects must be an array");

    std::vector<Operation> operations;
    static thread_local renderer::color::ColorPipeline fallbackPipeline;
    auto& pipeline = suppliedPipeline ? *suppliedPipeline : fallbackPipeline;
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
        if (type == kColorGradeType) {
            ColorGradeState state;
            if (!sampleColorGrade(effect, clipLocalFrame, state, output.diagnostics, error, options)) return false;
            std::vector<ColorGradeGpuStage> gpuStages;
            if (!buildColorGradeGpuPrograms(state, gpuStages, error)) return false;
            for (auto& stage : gpuStages) {
                operations.push_back({stage.domain == renderer::color::kGradingSpace ? GradeDomain::ACEScct : GradeDomain::ACEScg,
                    {}, {}, stage.domain, std::make_shared<const GpuProgram>(std::move(stage.program))});
            }
            const auto& params = effect.contains("params") ? effect["params"] : JsonValue::object();
            if (params.contains("lut")) {
                const auto& lut = params["lut"];
                if (!lut.is_object() || !lut.contains("id") || !lut["id"].is_string())
                    return invalid(id, "lut", "LUT requires a content ID");
                const auto lutId = lut["id"].get<std::string>();
                if (lutId.size() != 64 || !std::all_of(lutId.begin(), lutId.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) return invalid(id, "lut.id", "LUT ID must be a lowercase SHA-256");
                LutDomain domain;
                if (!lut.contains("domain") || !lut["domain"].is_string()
                    || !parseLutDomain(lut["domain"].get<std::string>(), domain))
                    return invalid(id, "lut.domain", "LUT requires an explicit rec709 or acescct domain");
                VulkanParameterValue amount;
                const JsonValue* payload = lut.contains("intensity") ? &lut["intensity"] : nullptr;
                if (payload && payload->is_object()) {
                    const auto validValue = [](const JsonValue& value) {
                        if (!value.is_number()) return false;
                        const double number = value.get<double>();
                        return std::isfinite(number) && number >= 0.0 && number <= 1.0
                            && (number == 0.0 || static_cast<float>(number) != 0.0f);
                    };
                    const auto validateIntensity = [&](const JsonValue& item) {
                        if (item.contains("value") && !validValue(item["value"])) return false;
                        if (!item.contains("track") || !item["track"].is_object()) return true;
                        const auto& track = item["track"];
                        if (!track.contains("keyframes") || !track["keyframes"].is_array()) return false;
                        for (const auto& key : track["keyframes"])
                            if (!key.is_object() || !key.contains("value") || !validValue(key["value"])) return false;
                        return true;
                    };
                    if (!validateIntensity(*payload)) return invalid(id, "lut.intensity", "LUT intensity and every keyframe must be 0..1");
                }
                if (!sampleParameter(payload, 1.0f, clipLocalFrame, amount, output.diagnostics, id, "lut.intensity", error, options)) return false;
                const float intensity = std::get<float>(amount);
                if (intensity < 0.0f || intensity > 1.0f) return invalid(id, "lut.intensity", "LUT intensity must be 0..1");
                if (intensity > 0.0f) {
                    std::string resourceError;
                    LutLibrary::Snapshot asset;
                    if (lutAssets) {
                        const auto found = lutAssets->find(lutId);
                        if (found != lutAssets->end()) asset = found->second;
                        else resourceError = "LUT resource was unavailable in the composition snapshot: " + lutId;
                    } else asset = LutLibrary::resolveConfigured(lutId, &resourceError);
                    if (!asset) {
                        if (!diagnostic(output, "missing_lut", id, "lut", resourceError, error, options.strict)) return false;
                    } else {
                        try {
                            const std::string space = domain == LutDomain::ACEScct ? renderer::color::kGradingSpace : renderer::color::kExportDisplay;
                            operations.push_back({domain == LutDomain::ACEScct ? GradeDomain::ACEScct : GradeDomain::ACEScg,
                                {}, {}, space, lutProgram(pipeline, asset, intensity)});
                        } catch (const std::exception& exception) {
                            if (!diagnostic(output, "lut_program_failed", id, "lut", exception.what(), error, options.strict)) return false;
                        }
                    }
                }
            }
            continue;
        }
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
            if (r < 0 || r > 32 || s < 0.0f) {
                return invalid(id, "radius", "Gaussian radius must be 0..32 and sigma must be nonnegative");
            }
            if (r != 0) {
                operations.push_back({GradeDomain::ACEScg, "grade_gaussian_blur_acescg",
                                      {{"blurRadius", r}, {"sigma", s}}});
            }
        }
    }

    std::vector<GraphStage> stages;
    auto document = VulkanGraphDocument::emptyStage();
    std::vector<ParameterUpdate> parameters;
    std::vector<ProgramUpdate> programs;
    auto previous = kVulkanGraphInputNodeId;
    std::string currentSpace = renderer::color::kWorkingSpace;
    auto appendProgram = [&](std::shared_ptr<const GpuProgram> program,
                             const std::string& inputSpace, const std::string& outputSpace) {
        const auto node = document.addProgramNode(program, contractFor(inputSpace), contractFor(outputSpace));
        if (!node || !document.connect({previous, 0}, {node, 0}))
            throw std::runtime_error("Cannot connect Grade GPU program");
        programs.push_back({node, std::move(program)});
        previous = node;
    };
    try {
        for (const auto& operation : operations) {
            const auto space = operation.colorSpace.empty()
                ? std::string(operation.domain == GradeDomain::ACEScct ? renderer::color::kGradingSpace : renderer::color::kWorkingSpace)
                : operation.colorSpace;
            if (stages.empty() || stages.back().colorSpace != space) {
                stages.emplace_back();
                stages.back().domain = operation.domain;
                stages.back().colorSpace = space;
                stages.back().document = VulkanGraphDocument::emptyStage(contractFor(space));
            }
            auto& stage = stages.back();
            const auto stagePrevious = stage.document.nodes().size() == 2
                ? kVulkanGraphInputNodeId : stage.document.nodes().back().id;
            VulkanGraphNodeId stageNode;
            if (operation.program) {
                stageNode = stage.document.addProgramNode(operation.program, contractFor(space), contractFor(space));
            } else {
                stageNode = stage.document.addNode(operation.filterId, operation.parameters, {}, contractFor(space), contractFor(space));
                stage.parameterUpdates.push_back({stageNode, operation.parameters});
            }
            if (!stageNode || !stage.document.connect({stagePrevious, 0}, {stageNode, 0}))
                throw std::runtime_error("Cannot construct Grade effect stage");
            if (currentSpace != space) {
                appendProgram(pipeline.processor(currentSpace, space).gpuProgramSnapshot(), currentSpace, space);
                currentSpace = space;
            }
            if (operation.program) appendProgram(operation.program, space, space);
            else {
                const auto node = document.addNode(operation.filterId, operation.parameters, {}, contractFor(space), contractFor(space));
                if (!node || !document.connect({previous, 0}, {node, 0}))
                    throw std::runtime_error("Cannot connect Grade effect node");
                parameters.push_back({node, operation.parameters});
                previous = node;
            }
        }
        for (auto& stage : stages) {
            if (!stage.document.connect({stage.document.nodes().back().id, 0}, {kVulkanGraphOutputNodeId, 0}))
                throw std::runtime_error("Cannot connect Grade stage output");
            stage.topologyKey = topology(stage);
        }
        if (currentSpace != renderer::color::kWorkingSpace)
            appendProgram(pipeline.processor(currentSpace, renderer::color::kWorkingSpace).gpuProgramSnapshot(),
                currentSpace, renderer::color::kWorkingSpace);
        if (!operations.empty()) {
            std::string graphError;
            if (!document.connect({previous, 0}, {kVulkanGraphOutputNodeId, 0}) || !document.validate(&graphError))
                throw std::runtime_error("Invalid complete Grade DAG: " + graphError);
            std::string key = "complete-grade-v1";
            for (const auto& node : document.nodes()) {
                key += "/" + std::to_string(node.id) + ":" + node.filterId;
                if (node.program) key += ":" + node.program->cacheId;
            }
            for (const auto& edge : document.edges())
                key += "/" + std::to_string(edge.output.nodeId) + ">" + std::to_string(edge.input.nodeId);
            output.topologyKey = std::move(key);
        }
    } catch (const std::exception& exception) {
        return diagnostic(output, "graph_construction_failed", {}, {}, exception.what(), error, true);
    }
    output.document = std::move(document);
    output.parameterUpdates = std::move(parameters);
    output.programUpdates = std::move(programs);
    output.stages = std::move(stages);
    return true;
}

} // namespace heisenberg::grade
