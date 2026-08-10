#include "VulkanColorInvertLayer.hpp"

#ifndef HEISENBERG_COLOR_INVERT_SHADER_PATH
#error HEISENBERG_COLOR_INVERT_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanColorInvertLayer::VulkanColorInvertLayer()
    : VulkanComputeLayer("VulkanColorInvert") {}

const char* VulkanColorInvertLayer::shaderPath() const {
    return HEISENBERG_COLOR_INVERT_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
