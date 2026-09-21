#include "VulkanMultiPassNode.hpp"

#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanMultiPassNode::VulkanMultiPassNode(
    const VulkanFilterDescriptor& descriptor,
    const VulkanGraphParameter& parameter)
    : VulkanGroupNode(descriptor.displayName) {
    if (descriptor.passes.empty()) {
        throw std::invalid_argument(
            "Manifest multi-pass filter must declare at least one pass");
    }

    for (const VulkanFilterPassDescriptor& pass : descriptor.passes) {
        VulkanGraphParameter passParameter = parameter;
        passParameter["directionX"] = pass.directionX;
        passParameter["directionY"] = pass.directionY;
        auto passNode = std::make_unique<VulkanManifestComputeNode>(
            descriptor, passParameter);
        addPass(std::move(passNode));
    }
}

} // namespace heisenberg::filtergraph
