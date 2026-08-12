#include "VulkanPipeGraph.hpp"

#include "VulkanNode.hpp"

#include <FilterGraph/Core/BaseLayer.hpp>
#include <Utiles/Logger.hpp>
#include <volk.h>

#include <algorithm>
#include <stdexcept>

namespace heisenberg::filtergraph {

VulkanPipeGraph::VulkanPipeGraph(const VulkanGraphContext& context)
    : PipeGraph(GpuType::vulkan), context_(context) {
    if (!initialize()) {
        throw std::runtime_error("Failed to initialize Vulkan filter graph");
    }
}

VulkanPipeGraph::~VulkanPipeGraph() {
    shutdown();
}

bool VulkanPipeGraph::initialize() {
    if (!context_.physicalDevice || !context_.device || !context_.queue
        || context_.queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        LOG_ERROR("FilterGraph: invalid Vulkan graph context");
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilyIndex;
    if (vkCreateCommandPool(context_.device, &poolInfo, nullptr, &commandPool_)
        != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo commandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(context_.device, &commandInfo, &commandBuffer_)
        != VK_SUCCESS) {
        shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(context_.device, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    VkSemaphoreTypeCreateInfo typeInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &typeInfo;
    if (vkCreateSemaphore(context_.device, &semaphoreInfo, nullptr, &timeline_)
        != VK_SUCCESS) {
        shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void VulkanPipeGraph::shutdown() {
    if (!context_.device) return;
    if (submitted_) waitForPreviousSubmission();
    if (context_.queue) vkQueueWaitIdle(context_.queue);
    if (timeline_) vkDestroySemaphore(context_.device, timeline_, nullptr);
    if (fence_) vkDestroyFence(context_.device, fence_, nullptr);
    if (commandPool_) vkDestroyCommandPool(context_.device, commandPool_, nullptr);
    timeline_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    commandBuffer_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    initialized_ = false;
}

bool VulkanPipeGraph::waitForPreviousSubmission() {
    if (!submitted_) return true;
    const VkResult result = vkWaitForFences(
        context_.device, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: waiting for the previous frame failed ({})",
                  static_cast<int>(result));
        return false;
    }
    submitted_ = false;
    return true;
}

bool VulkanPipeGraph::onGraphRebuilt() {
    if (!initialized_ || !waitForPreviousSubmission()) return false;

    // Structural changes are rare. Waiting here keeps old node images alive
    // until libplacebo has returned every borrowed graph output.
    vkQueueWaitIdle(context_.queue);
    for (BaseLayer* base : nodes()) {
        auto* layer = dynamic_cast<VulkanNode*>(base);
        if (!layer || !layer->prepare(context_)) {
            LOG_ERROR("FilterGraph: failed to prepare Vulkan node '{}'",
                      base ? base->getMark() : "<null>");
            return false;
        }
    }
    return true;
}

void VulkanPipeGraph::onGraphCleared() {
    if (context_.queue) vkQueueWaitIdle(context_.queue);
}

bool VulkanPipeGraph::appendWait(
    std::vector<VulkanSyncPoint>& waits, VulkanSyncPoint wait) const {
    if (!wait.valid()) return true;
    auto found = std::find_if(waits.begin(), waits.end(),
        [&](const VulkanSyncPoint& item) {
            return item.semaphore == wait.semaphore;
        });
    if (found == waits.end()) {
        waits.push_back(wait);
    } else {
        found->value = std::max(found->value, wait.value);
    }
    return true;
}

bool VulkanPipeGraph::onRun(const FrameContext& frame) {
    if (!initialized_ || nodes().empty() || !waitForPreviousSubmission()) {
        return false;
    }

    std::vector<VulkanSyncPoint> waits;
    for (BaseLayer* base : nodes()) {
        auto* layer = static_cast<VulkanNode*>(base);
        appendWait(waits, layer->takeConsumerDone());
    }

    vkResetFences(context_.device, 1, &fence_);
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    for (int32_t nodeIndex : executionOrder()) {
        auto* layer = static_cast<VulkanNode*>(
            nodes()[static_cast<size_t>(nodeIndex)]);
        std::vector<VulkanImageRef> inputs;
        inputs.reserve(static_cast<size_t>(layer->inputCount()));
        for (int32_t pin = 0; pin < layer->inputCount(); ++pin) {
            const GraphEdge* edge = inputEdge(nodeIndex, pin);
            if (!edge) return false;
            auto* source = static_cast<VulkanNode*>(
                nodes()[static_cast<size_t>(edge->fromNode)]);
            const VulkanImageRef& sourceImage = source->output(edge->fromPin);
            if (!sourceImage.valid()) return false;
            inputs.push_back(sourceImage);
        }
        layer->bindInputs(std::move(inputs));
        if (layer->beginFrame(frame)) {
            layer->record(commandBuffer_, frame);
        }

        for (int32_t outputPin = 0; outputPin < layer->outputCount(); ++outputPin) {
            appendWait(waits, layer->output(outputPin).ready);
        }
    }

    if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) return false;

    std::vector<VkSemaphore> waitSemaphores;
    std::vector<uint64_t> waitValues;
    std::vector<VkPipelineStageFlags> waitStages;
    waitSemaphores.reserve(waits.size());
    waitValues.reserve(waits.size());
    waitStages.reserve(waits.size());
    for (const VulkanSyncPoint& wait : waits) {
        waitSemaphores.push_back(wait.semaphore);
        waitValues.push_back(wait.value);
        waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    const uint64_t signalValue = ++timelineValue_;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount =
        static_cast<uint32_t>(waitValues.size());
    timelineInfo.pWaitSemaphoreValues = waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount =
        static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &timeline_;

    const VkResult submitResult =
        vkQueueSubmit(context_.queue, 1, &submitInfo, fence_);
    if (submitResult != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: vkQueueSubmit failed ({})",
                  static_cast<int>(submitResult));
        return false;
    }
    submitted_ = true;

    const VulkanSyncPoint completion{timeline_, signalValue};
    for (BaseLayer* base : nodes()) {
        static_cast<VulkanNode*>(base)->setCompletion(completion);
    }
    return true;
}

} // namespace heisenberg::filtergraph
