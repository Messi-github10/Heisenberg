#pragma once

#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanColorInvertNode final : public VulkanComputeNode {
public:
    VulkanColorInvertNode();

protected:
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
