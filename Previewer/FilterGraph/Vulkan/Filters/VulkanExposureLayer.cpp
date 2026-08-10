#include "VulkanExposureLayer.hpp"

#ifndef HEISENBERG_EXPOSURE_SHADER_PATH
#error HEISENBERG_EXPOSURE_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanExposureLayer::VulkanExposureLayer()
    : VulkanComputeLayer("VulkanExposure") {
    paramet = 0.0f;
    oldParamet = paramet;
    setUniformBufferSize(sizeof(paramet));
    updateUniformData(paramet);
}

void VulkanExposureLayer::onUpdateParamet() {
    if (paramet == oldParamet) return;
    updateUniformData(paramet);
}

const char* VulkanExposureLayer::shaderPath() const {
    return HEISENBERG_EXPOSURE_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
