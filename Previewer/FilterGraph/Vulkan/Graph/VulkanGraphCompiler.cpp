#include "VulkanGraphCompiler.hpp"

#include "VulkanNodeFactory.hpp"

#include <FilterGraph/Interface/ILayerFactory.hpp>
#include <FilterGraph/Interface/IPipeGraph.hpp>

#include <utility>
#include <variant>

namespace heisenberg::filtergraph {
namespace {

void setError(std::string* error, const char* message) {
    if (error) *error = message;
}

IBaseLayer* createRuntimeNode(
    const VulkanGraphNodeDesc& node,
    VulkanNodeFactory& nodeFactory,
    IInputLayer*& input,
    IOutputLayer*& output) {
    switch (node.type) {
        case VulkanGraphNodeType::input:
            input = nodeFactory.createInput();
            return input ? input->getLayer() : nullptr;
        case VulkanGraphNodeType::output:
            output = nodeFactory.createOutput();
            return output ? output->getLayer() : nullptr;
        case VulkanGraphNodeType::colorInvert: {
            ILayer* layer = nodeFactory.createColorInvert();
            return layer ? layer->getLayer() : nullptr;
        }
        case VulkanGraphNodeType::exposure: {
            auto* layer = nodeFactory.createExposure();
            if (!layer) return nullptr;
            layer->updateParamet(std::get<ExposureParamet>(node.parameter).exposure);
            return layer->getLayer();
        }
        case VulkanGraphNodeType::blend: {
            auto* layer = nodeFactory.createBlend();
            if (!layer) return nullptr;
            layer->updateParamet(std::get<BlendParamet>(node.parameter).factor);
            return layer->getLayer();
        }
        case VulkanGraphNodeType::gaussianBlur: {
            auto* layer = nodeFactory.createGaussianBlur();
            if (!layer) return nullptr;
            layer->updateParamet(
                std::get<GaussianBlurParamet>(node.parameter));
            return layer->getLayer();
        }
    }
    return nullptr;
}

} // namespace

bool VulkanGraphCompiler::compile(
    const VulkanGraphDocument& document,
    VulkanNodeFactory& nodeFactory,
    IPipeGraph& graph,
    VulkanCompiledGraph& result,
    std::string* error) {
    if (!document.validate(error)) return false;

    VulkanCompiledGraph compiled;
    compiled.nodes.reserve(document.nodes().size());
    for (const VulkanGraphNodeDesc& node : document.nodes()) {
        IBaseLayer* runtimeNode = createRuntimeNode(
            node, nodeFactory, compiled.input, compiled.output);
        if (!runtimeNode || !graph.addNode(runtimeNode)) {
            setError(error, "Failed to create or add a Vulkan graph node");
            return false;
        }
        compiled.nodes.emplace(node.id, runtimeNode);
    }

    for (const VulkanGraphEdgeDesc& edge : document.edges()) {
        const auto source = compiled.nodes.find(edge.fromNode);
        const auto destination = compiled.nodes.find(edge.toNode);
        if (source == compiled.nodes.end()
            || destination == compiled.nodes.end()
            || !graph.addLine(source->second, destination->second,
                              edge.fromPin, edge.toPin)) {
            setError(error, "Failed to connect a Vulkan graph edge");
            return false;
        }
    }

    if (!compiled.input || !compiled.output) {
        setError(error, "Compiled Vulkan graph has no input or output");
        return false;
    }
    result = std::move(compiled);
    return true;
}

} // namespace heisenberg::filtergraph
