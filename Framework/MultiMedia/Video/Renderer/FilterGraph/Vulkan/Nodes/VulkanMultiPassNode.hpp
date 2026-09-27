#pragma once

#include "VulkanGroupNode.hpp"
#include "VulkanManifestComputeNode.hpp"

#include <vector>

namespace heisenberg::filtergraph {

class VulkanMultiPassNode final : public VulkanGroupNode {
public:
    VulkanMultiPassNode(const VulkanFilterDescriptor& descriptor,
                        const VulkanGraphParameter& parameter);

    bool setParameters(const VulkanGraphParameter& parameter);

private:
    VulkanFilterDescriptor descriptor_;
    std::vector<VulkanManifestComputeNode*> passNodes_;
};

} // namespace heisenberg::filtergraph
