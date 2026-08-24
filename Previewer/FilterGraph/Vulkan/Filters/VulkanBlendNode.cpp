#include "VulkanBlendNode.hpp"

#include "../ShaderUniforms.hpp"

#ifndef HEISENBERG_BLEND_SHADER_PATH
#error HEISENBERG_BLEND_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanBlendNode::VulkanBlendNode()
    : VulkanComputeNode("VulkanBlend", 2, 1) {
    paramet = 0.5f;
    oldParamet = paramet;
    setUniformBufferSize(sizeof(shader_abi::ScalarUniform));
    updateUniformData(shader_abi::ScalarUniform{paramet});
}

bool VulkanBlendNode::configureOutputs(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 2 || inputs[0] != inputs[1]) return false;
    return VulkanComputeNode::configureOutputs(inputs);
}

void VulkanBlendNode::onUpdateParamet() {
    if (paramet == oldParamet) return;
    updateUniformData(shader_abi::ScalarUniform{paramet});
}

const char* VulkanBlendNode::shaderPath() const {
    return HEISENBERG_BLEND_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
