#include "VulkanGroupNode.hpp"
#include <Utiles/Logger.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/ResourceManager.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/StateTransitionScheduler.hpp>
#include <volk.h>
#include <stdexcept>
#include <utility>
#include <cstddef>
#include <string>

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
        pass->setImageContracts(inputContract(context.imageContract),
                                outputContract(context.imageContract));
        if (!pass->prepare(context)) return false;
    }
    return true;
}

void VulkanGroupNode::record(
    VkCommandBuffer commandBuffer, const FrameContext& frame) {
    VulkanImageRef current = input(0);
    LogicalResourceId currentResource = inputResource(0);
    for (const std::unique_ptr<VulkanNode>& pass : passes_) {
        pass->setResourceManager(resourceManager());
        pass->bindInputResources({currentResource});
        if (pass->beginFrame(frame)) {
            for (const auto& access : pass->declareResourceAccess()) {
                const auto image = resourceManager()->getResource(access.resource);
                const auto state = resourceManager()->getState(access.resource);
                StateTransition transition{access.resource, state, access.expectedState, 0};
                StateTransitionScheduler::executeTransition(commandBuffer, image.image, transition);
                resourceManager()->setState(access.resource, access.expectedState);
            }
            pass->record(commandBuffer, frame);
        }
        currentResource = pass->logicalOutputResource(0);
        current = pass->output(0);
        if (!currentResource.valid() || !current.valid()) {
            LOG_ERROR("FilterGraph: group pass '{}' produced no output",
                      pass->getMark());
            return;
        }
    }
}

void VulkanGroupNode::setCompletion(VulkanSyncPoint completion) {
    VulkanNode::setCompletion(completion);
    for (const auto& pass : passes_) pass->setCompletion(completion);
}

std::vector<ResourceAccess> VulkanGroupNode::declareResourceAccess() const {
    // Only the boundary input belongs to the enclosing graph's transition list.
    // Scratch/output images transition immediately before their respective pass.
    ResourceAccess inputAccess;
    inputAccess.resource = inputResource(0);
    inputAccess.mode = ResourceAccessMode::Read;
    inputAccess.expectedState.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputAccess.expectedState.stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    inputAccess.expectedState.access = VK_ACCESS_SHADER_READ_BIT;
    return {inputAccess};
}

LogicalResourceId VulkanGroupNode::logicalOutputResource(int32_t index) const {
    if (index != 0) return {};
    if (!active()) return inputResource(0);
    if (passes_.empty()) return {};
    return passes_.back()->logicalOutputResource(0);
}

std::vector<LogicalResourceRequest>
VulkanGroupNode::declareResourceRequests() const {
    std::vector<LogicalResourceRequest> requests;
    for (size_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        auto passRequests = passes_[passIndex]->declareResourceRequests();
        for (auto& request : passRequests) {
            const std::string localName = request.outputPin >= 0
                ? std::string("output:") + std::to_string(request.outputPin)
                : request.name;
            request.outputPin = -1;
            request.name = std::string("pass:") + std::to_string(passIndex)
                + ":" + localName;
            requests.push_back(std::move(request));
        }
    }
    return requests;
}

void VulkanGroupNode::bindDeclaredResources(
    const std::vector<LogicalResourceId>& resources) {
    size_t offset = 0;
    for (const auto& pass : passes_) {
        pass->setResourceManager(resourceManager());
        const size_t count = pass->declareResourceRequests().size();
        if (offset + count > resources.size()) break;
        pass->bindDeclaredResources(std::vector<LogicalResourceId>(
            resources.begin() + static_cast<std::ptrdiff_t>(offset),
            resources.begin() + static_cast<std::ptrdiff_t>(offset + count)));
        offset += count;
    }
}

} // namespace heisenberg::filtergraph
