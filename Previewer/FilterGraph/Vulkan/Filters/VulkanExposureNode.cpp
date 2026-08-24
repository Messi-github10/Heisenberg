#include "VulkanExposureNode.hpp"

#include "../ShaderUniforms.hpp"

#ifndef HEISENBERG_EXPOSURE_SHADER_PATH
#error HEISENBERG_EXPOSURE_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanExposureNode::VulkanExposureNode()
    : VulkanComputeNode("VulkanExposure") {
    paramet = 0.0f;
    oldParamet = paramet;
    setUniformBufferSize(sizeof(shader_abi::ScalarUniform));
    updateUniformData(shader_abi::ScalarUniform{paramet});
}

void VulkanExposureNode::onUpdateParamet() {
    if (paramet == oldParamet) return;
    updateUniformData(shader_abi::ScalarUniform{paramet});
}

const char* VulkanExposureNode::shaderPath() const {
    return HEISENBERG_EXPOSURE_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
