#include "VulkanLayer.hpp"

#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanLayer::VulkanLayer(std::string mark, int32_t inputCount, int32_t outputCount)
    : BaseLayer(std::move(mark), inputCount, outputCount),
      inputs_(static_cast<size_t>(inputCount)),
      outputs_(static_cast<size_t>(outputCount)) {}

void VulkanLayer::bindInputs(std::vector<VulkanImageRef> inputs) {
    if (inputs.size() != inputs_.size()) {
        throw std::invalid_argument("FilterGraph Vulkan input count mismatch");
    }
    inputs_ = std::move(inputs);
}

const VulkanImageRef& VulkanLayer::input(int32_t index) const {
    return inputs_.at(static_cast<size_t>(index));
}

const VulkanImageRef& VulkanLayer::output(int32_t index) const {
    return outputs_.at(static_cast<size_t>(index));
}

void VulkanLayer::setOutput(int32_t index, const VulkanImageRef& image) {
    outputs_.at(static_cast<size_t>(index)) = image;
}

bool VulkanLayer::beginFrame(const FrameContext&) {
    if (!active()) {
        bypass();
        return false;
    }
    return true;
}

void VulkanLayer::setCompletion(VulkanSyncPoint completion) {
    for (VulkanImageRef& outputImage : outputs_) {
        if (outputImage.valid()) outputImage.ready = completion;
    }
}

void VulkanLayer::bypass() {
    if (inputCount() != 1 || outputCount() != 1 || !input(0).valid()) {
        throw std::runtime_error(
            "FilterGraph can only bypass a one-input, one-output layer");
    }
    setOutput(0, input(0));
}

} // namespace heisenberg::filtergraph
