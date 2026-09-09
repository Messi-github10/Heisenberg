#include "VulkanNode.hpp"
#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanNode::VulkanNode(std::string mark, int32_t inputCount, int32_t outputCount)
    : BaseNode(std::move(mark), inputCount, outputCount),
      inputs_(static_cast<size_t>(inputCount)),
      outputs_(static_cast<size_t>(outputCount)) {}

void VulkanNode::bindInputs(std::vector<VulkanImageRef> inputs) {
    if (inputs.size() != inputs_.size()) {
        throw std::invalid_argument("FilterGraph Vulkan input count mismatch");
    }
    inputs_ = std::move(inputs);
}

const VulkanImageRef& VulkanNode::input(int32_t index) const {
    return inputs_.at(static_cast<size_t>(index));
}

const VulkanImageRef& VulkanNode::output(int32_t index) const {
    return outputs_.at(static_cast<size_t>(index));
}

void VulkanNode::setOutput(int32_t index, const VulkanImageRef& image) {
    outputs_.at(static_cast<size_t>(index)) = image;
}

bool VulkanNode::beginFrame(const FrameContext&) {
    if (!active()) {
        bypass();
        return false;
    }
    return true;
}

void VulkanNode::setCompletion(VulkanSyncPoint completion) {
    for (VulkanImageRef& outputImage : outputs_) {
        if (outputImage.valid()) outputImage.ready = completion;
    }
}

void VulkanNode::bypass() {
    if (inputCount() != 1 || outputCount() != 1 || !input(0).valid()) {
        throw std::runtime_error(
            "FilterGraph can only bypass a one-input, one-output node");
    }
    setOutput(0, input(0));
}

} // namespace heisenberg::filtergraph
