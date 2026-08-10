#include "VulkanBlendLayer.hpp"

#ifndef HEISENBERG_BLEND_SHADER_PATH
#error HEISENBERG_BLEND_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanBlendLayer::VulkanBlendLayer()
    : VulkanComputeLayer("VulkanBlend", 2, 1) {
    paramet = 0.5f;
    oldParamet = paramet;
    setUniformBufferSize(sizeof(paramet));
    updateUniformData(paramet);
}

bool VulkanBlendLayer::configureOutputs(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 2 || inputs[0] != inputs[1]) return false;
    return VulkanComputeLayer::configureOutputs(inputs);
}

void VulkanBlendLayer::onUpdateParamet() {
    if (paramet == oldParamet) return;
    updateUniformData(paramet);
}

const char* VulkanBlendLayer::shaderPath() const {
    return HEISENBERG_BLEND_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
