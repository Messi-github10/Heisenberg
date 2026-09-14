#include "VulkanPipeGraph.hpp"
#include "VulkanNode.hpp"
#include "ResourceManager.hpp"
#include "StateTransitionScheduler.hpp"
#include <Video/Renderer/FilterGraph/Core/BaseNode.hpp>
#include <Utiles/Logger.hpp>
#include <volk.h>
#include <algorithm>
#include <stdexcept>

namespace heisenberg::filtergraph {

VulkanPipeGraph::VulkanPipeGraph(const VulkanGraphContext& context)
    : PipeGraph(GraphicApiBackend::vulkan), context_(context) {
    resourceManager_ = std::make_unique<ResourceManager>(context);
    transitionScheduler_ = std::make_unique<StateTransitionScheduler>();
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

    // 开始 rebuild，把所有 Logical 条目标记为 stale
    resourceManager_->beginRebuild();
    transitionScheduler_->clear();

    for (BaseNode* base : nodes()) {
        auto* node = dynamic_cast<VulkanNode*>(base);
        if (!node) continue;

        // 设置 ResourceManager 引用
        node->setResourceManager(resourceManager_.get());

        if (!node->prepare(context_)) {
            LOG_ERROR("FilterGraph: failed to prepare Vulkan node '{}'",
                      base ? base->getMark() : "<null>");
            return false;
        }
    }

    // 构建状态转换计划
    if (!buildTransitionPlan()) {
        LOG_ERROR("FilterGraph: failed to build state transition plan");
        return false;
    }

    // 删除本次 rebuild 未被访问到的 stale 条目（已从图中移除的 Node）
    resourceManager_->endRebuild();

    return true;
}

void VulkanPipeGraph::onGraphCleared() {
    if (context_.queue) vkQueueWaitIdle(context_.queue);
    resourceManager_->clear();
    transitionScheduler_->clear();
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
    for (BaseNode* base : nodes()) {
        auto* node = static_cast<VulkanNode*>(base);
        appendWait(waits, node->takeConsumerDone());
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
        auto* node = static_cast<VulkanNode*>(
            nodes()[static_cast<size_t>(nodeIndex)]);

        // 在 Node 执行前插入状态转换
        const auto& transitions = transitionScheduler_->getTransitionsBefore(
            static_cast<uint32_t>(nodeIndex));
        for (const auto& transition : transitions) {
            VulkanImageRef imageRef = resourceManager_->getResource(transition.resource);
            if (imageRef.valid()) {
                StateTransitionScheduler::executeTransition(
                    commandBuffer_, imageRef.image, transition);
                // 更新资源管理器中的状态
                resourceManager_->setState(transition.resource, transition.newState);
            }
        }

        for (int32_t pin = 0; pin < node->inputCount(); ++pin) {
            const RuntimeGraphEdge* edge = inputEdge(nodeIndex, pin);
            if (!edge) return false;
            auto* source = static_cast<VulkanNode*>(
                nodes()[static_cast<size_t>(edge->fromNode)]);
            const LogicalResourceId id = source->logicalOutputResource(edge->fromPin);
            if (!id.valid() || !resourceManager_->getResource(id).valid()) return false;
        }
        if (node->beginFrame(frame)) {
            node->record(commandBuffer_, frame);
        }

        for (int32_t outputPin = 0; outputPin < node->outputCount(); ++outputPin) {
            appendWait(waits, node->output(outputPin).ready);
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
    for (BaseNode* base : nodes()) {
        static_cast<VulkanNode*>(base)->setCompletion(completion);
    }
    return true;
}

bool VulkanPipeGraph::buildTransitionPlan() {
    // 构建状态转换计划
    // 遍历执行顺序中的每个 Node，记录资源访问并生成状态转换

    for (size_t i = 0; i < executionOrder().size(); ++i) {
        int32_t nodeIndex = executionOrder()[i];
        auto* node = static_cast<VulkanNode*>(nodes()[static_cast<size_t>(nodeIndex)]);

        // 先绑定边上的 Logical ID，再由 Graph 消费 Node 的资源声明
        std::vector<LogicalResourceId> inputResources;
        inputResources.reserve(static_cast<size_t>(node->inputCount()));
        for (int32_t pin = 0; pin < node->inputCount(); ++pin) {
            const RuntimeGraphEdge* edge = inputEdge(nodeIndex, pin);
            if (!edge) return false;
            auto* source = static_cast<VulkanNode*>(
                nodes()[static_cast<size_t>(edge->fromNode)]);
            inputResources.push_back(source->logicalOutputResource(edge->fromPin));
        }
        node->bindInputResources(inputResources);

        const bool bypassed = !node->active();
        if (bypassed
            && (node->inputCount() != 1 || node->outputCount() != 1
                || !inputResources[0].valid())) {
            LOG_ERROR("FilterGraph: disabled node '{}' cannot be bypassed",
                      node->getMark());
            return false;
        }

        const auto requests = node->declareResourceRequests();
        std::vector<LogicalResourceId> resources;
        resources.reserve(requests.size());
        for (const auto& request : requests) {
            LogicalResourceDesc desc;
            desc.kind = request.kind;
            desc.extent = request.extent;
            desc.usage = request.usage;
            desc.contract = request.contract;
            if (request.outputPin >= node->outputCount()) return false;
            desc.producerNode = static_cast<uint32_t>(nodeIndex);
            desc.producerPin = request.outputPin;
            desc.identity = request.outputPin >= 0
                ? std::string("output:") + std::to_string(request.outputPin)
                : request.name;
            if (desc.identity.empty()) return false;
            const std::string key = std::to_string(nodeIndex) + ":" + desc.identity;
            LogicalResourceId id = resourceManager_->findOrAllocateLogicalResource(
                key, desc);
            if (!id.valid()) return false;
            if (request.kind == LogicalResourceKind::External
                && request.imported.valid()
                && !resourceManager_->updateImported(id, request.imported)) {
                return false;
            }
            resources.push_back(id);
        }
        node->bindDeclaredResources(resources);
        if (bypassed) continue;

        // 获取 Node 的资源访问声明
        auto accesses = node->declareResourceAccess();

        for (const auto& access : accesses) {
            if (!access.resource.valid()) continue;

            // 获取资源当前状态
            auto currentState = resourceManager_->getState(access.resource);

            // 记录访问并生成转换（如果需要）
            auto transition = transitionScheduler_->recordAccess(
                static_cast<uint32_t>(nodeIndex), access, currentState);

            // 如果需要状态转换，更新资源状态
            if (transition.oldState != transition.newState) {
                resourceManager_->setState(access.resource, transition.newState);
            }
        }
    }

    LOG_INFO("FilterGraph: transition plan built successfully");
    return true;
}

} // namespace heisenberg::filtergraph
