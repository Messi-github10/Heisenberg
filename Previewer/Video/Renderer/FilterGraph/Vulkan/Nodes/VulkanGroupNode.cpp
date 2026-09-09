#include "VulkanGroupNode.hpp"
#include <Utiles/Logger.hpp>
#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanGroupNode::VulkanGroupNode(std::string mark)
    : VulkanNode(std::move(mark), 1, 1) {}

VulkanGroupNode::~VulkanGroupNode() = default;

VulkanNode* VulkanGroupNode::addPass(
    std::unique_ptr<VulkanNode> pass) {
    if (!pass || pass->inputCount() != 1 || pass->outputCount() != 1) {
        throw std::invalid_argument(
            "FilterGraph group node passes must have one input and one output");
    }
    VulkanNode* result = pass.get();
    passes_.push_back(std::move(pass));
    return result;
}

bool VulkanGroupNode::configure(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || passes_.empty()) return false;

    ImageFormat current = inputs[0];
    setInputFormat(0, current);
    for (const std::unique_ptr<VulkanNode>& pass : passes_) {
        if (!pass->configureAsGroupPass({current})
            || pass->outputFormats().size() != 1) {
            return false;
        }
        current = pass->outputFormats()[0];
    }
    setOutputFormat(0, current);
    return true;
}

bool VulkanGroupNode::prepare(const VulkanGraphContext& context) {
    for (const std::unique_ptr<VulkanNode>& pass : passes_) {
        if (!pass->prepare(context)) return false;
    }
    return true;
}

void VulkanGroupNode::record(
    VkCommandBuffer commandBuffer, const FrameContext& frame) {
    setOutput(0, {});
    VulkanImageRef current = input(0);
    for (const std::unique_ptr<VulkanNode>& pass : passes_) {
        pass->bindInputs({current});
        if (pass->beginFrame(frame)) {
            pass->record(commandBuffer, frame);
        }
        current = pass->output(0);
        if (!current.valid()) {
            LOG_ERROR("FilterGraph: group pass '{}' produced no output",
                      pass->getMark());
            return;
        }
    }
    setOutput(0, current);
}

std::vector<ResourceAccess> VulkanGroupNode::declareResourceAccess() const {
    std::vector<ResourceAccess> accesses;
    LogicalResourceId previous;
    if (inputCount() > 0) previous = inputResource(0);

    for (const auto& pass : passes_) {
        auto* mutablePass = const_cast<VulkanNode*>(pass.get());
        mutablePass->bindInputResources({previous});
        auto passAccesses = pass->declareResourceAccess();
        accesses.insert(accesses.end(), passAccesses.begin(), passAccesses.end());
        previous = pass->logicalOutputResource(0);
    }

    return accesses;
}

LogicalResourceId VulkanGroupNode::logicalOutputResource(int32_t index) const {
    if (index != 0 || passes_.empty()) return {};
    return passes_.back()->logicalOutputResource(0);
}

bool VulkanGroupNode::allocateResources(ResourceManager& manager) {
    // 让所有子 pass 分配资源
    for (const auto& pass : passes_) {
        pass->setResourceManager(&manager);
        if (!pass->allocateResources(manager)) {
            LOG_ERROR("FilterGraph: group pass '{}' failed to allocate resources",
                      pass->getMark());
            return false;
        }
    }
    return true;
}

} // namespace heisenberg::filtergraph
