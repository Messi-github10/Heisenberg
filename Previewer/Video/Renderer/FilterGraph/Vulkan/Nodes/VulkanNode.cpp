#include "VulkanNode.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Graph/ResourceManager.hpp>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanNode::VulkanNode(std::string mark, int32_t inputCount, int32_t outputCount)
    : BaseNode(std::move(mark), inputCount, outputCount),
      inputResources_(static_cast<size_t>(inputCount)),
      outputResources_(static_cast<size_t>(outputCount)) {}

void VulkanNode::bindInputResources(std::vector<LogicalResourceId> resources) {
    if (resources.size() != inputResources_.size()) {
        throw std::invalid_argument("FilterGraph Vulkan input count mismatch");
    }
    inputResources_ = std::move(resources);
}

void VulkanNode::bindDeclaredResources(
    const std::vector<LogicalResourceId>& resources) {
    if (resources.size() < outputResources_.size()) {
        throw std::invalid_argument("FilterGraph Vulkan output resource count mismatch");
    }
    std::copy_n(resources.begin(), outputResources_.size(), outputResources_.begin());
}

LogicalResourceId VulkanNode::inputResource(int32_t index) const {
    return inputResources_.at(static_cast<size_t>(index));
}

LogicalResourceId VulkanNode::logicalOutputResource(int32_t index) const {
    if (!active() && inputCount() == 1 && outputCount() == 1 && index == 0) {
        return inputResource(0);
    }
    return outputResources_.at(static_cast<size_t>(index));
}

VulkanImageRef VulkanNode::input(int32_t index) const {
    return resourceManager_ ? resourceManager_->getResource(inputResource(index))
                            : VulkanImageRef{};
}

VulkanImageRef VulkanNode::output(int32_t index) const {
    return resourceManager_
        ? resourceManager_->getResource(logicalOutputResource(index))
        : VulkanImageRef{};
}

bool VulkanNode::beginFrame(const FrameContext&) {
    return active();
}

void VulkanNode::setCompletion(VulkanSyncPoint completion) {
    if (!active() || !resourceManager_) return;
    for (const LogicalResourceId id : outputResources_) {
        if (id.valid()) resourceManager_->setReady(id, completion);
    }
}

} // namespace heisenberg::filtergraph
