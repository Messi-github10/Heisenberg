#pragma once

#include "VulkanGroupNode.hpp"
#include "VulkanManifestComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanMultiPassNode final : public VulkanGroupNode {
public:
    VulkanMultiPassNode(const VulkanFilterDescriptor& descriptor,
                        const VulkanGraphParameter& parameter);
};

} // namespace heisenberg::filtergraph
