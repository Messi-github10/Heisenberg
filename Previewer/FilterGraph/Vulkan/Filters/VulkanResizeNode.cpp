#include "VulkanResizeNode.hpp"

#ifndef HEISENBERG_RESIZE_SHADER_PATH
#error HEISENBERG_RESIZE_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanResizeNode::VulkanResizeNode()
    : VulkanComputeNode("VulkanResize") {
    paramet = {};
    oldParamet = paramet;
}

bool VulkanResizeNode::configureOutputs(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || paramet.width <= 0 || paramet.height <= 0) {
        return false;
    }
    ImageFormat output = inputs[0];
    output.width = paramet.width;
    output.height = paramet.height;
    setOutputFormat(0, output);
    return true;
}

VulkanInputBinding VulkanResizeNode::inputBinding(int32_t) const {
    return VulkanInputBinding::sampledLinear;
}

void VulkanResizeNode::onUpdateParamet() {
    if (paramet == oldParamet || paramet.width <= 0 || paramet.height <= 0) {
        return;
    }
    invalidateGraph();
}

const char* VulkanResizeNode::shaderPath() const {
    return HEISENBERG_RESIZE_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
