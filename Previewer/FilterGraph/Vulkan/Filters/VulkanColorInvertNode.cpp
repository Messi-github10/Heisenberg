#include "VulkanColorInvertNode.hpp"

#ifndef HEISENBERG_COLOR_INVERT_SHADER_PATH
#error HEISENBERG_COLOR_INVERT_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {

VulkanColorInvertNode::VulkanColorInvertNode()
    : VulkanComputeNode("VulkanColorInvert") {}

const char* VulkanColorInvertNode::shaderPath() const {
    return HEISENBERG_COLOR_INVERT_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
