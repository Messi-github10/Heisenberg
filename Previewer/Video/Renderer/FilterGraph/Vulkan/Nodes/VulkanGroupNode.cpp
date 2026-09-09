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

} // namespace heisenberg::filtergraph
