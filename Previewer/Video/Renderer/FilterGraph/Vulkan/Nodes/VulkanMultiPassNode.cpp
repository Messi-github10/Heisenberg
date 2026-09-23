#include "VulkanMultiPassNode.hpp"

#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanMultiPassNode::VulkanMultiPassNode(
    const VulkanFilterDescriptor& descriptor,
    const VulkanGraphParameter& parameter)
    : VulkanGroupNode(descriptor.displayName),
      descriptor_(descriptor) {
    if (descriptor_.passes.empty()) {
        throw std::invalid_argument(
            "Manifest multi-pass filter must declare at least one pass");
    }

    for (const VulkanFilterPassDescriptor& pass : descriptor_.passes) {
        VulkanGraphParameter passParameter = parameter;
        passParameter["directionX"] = pass.directionX;
        passParameter["directionY"] = pass.directionY;
        auto passNode = std::make_unique<VulkanManifestComputeNode>(
            descriptor_, passParameter);
        passNodes_.push_back(passNode.get());
        addPass(std::move(passNode));
    }
}

bool VulkanMultiPassNode::setParameters(const VulkanGraphParameter& parameter) {
    if (passNodes_.size() != descriptor_.passes.size()) return false;
    for (size_t index = 0; index < passNodes_.size(); ++index) {
        VulkanGraphParameter passParameter = parameter;
        passParameter["directionX"] = descriptor_.passes[index].directionX;
        passParameter["directionY"] = descriptor_.passes[index].directionY;
        if (!passNodes_[index]->setParameters(passParameter)) return false;
    }
    return true;
}

} // namespace heisenberg::filtergraph
