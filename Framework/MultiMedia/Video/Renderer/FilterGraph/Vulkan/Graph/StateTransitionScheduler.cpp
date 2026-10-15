#include "StateTransitionScheduler.hpp"
#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {

std::vector<StateTransition> StateTransitionScheduler::emptyTransitions_;

StateTransition StateTransitionScheduler::recordAccess(
    uint32_t nodeIndex,
    const ResourceAccess& access,
    const LogicalResourceState& currentState) {

    StateTransition transition;
    transition.resource = access.resource;
    transition.oldState = currentState;
    transition.newState = access.expectedState;
    transition.beforeNodeIndex = nodeIndex;

    // 如果状态相同，不需要转换
    if (currentState == access.expectedState) {
        return transition;
    }

    // 记录需要在该 Node 之前执行的转换
    transitions_[nodeIndex].push_back(transition);

    return transition;
}

const std::vector<StateTransition>& StateTransitionScheduler::getTransitionsBefore(
    uint32_t nodeIndex) const {
    auto it = transitions_.find(nodeIndex);
    if (it == transitions_.end()) {
        return emptyTransitions_;
    }
    return it->second;
}

void StateTransitionScheduler::clear() {
    transitions_.clear();
}

void StateTransitionScheduler::executeTransition(
    VkCommandBuffer commandBuffer,
    VkImage image,
    const StateTransition& transition) {

    // 如果状态相同，不需要执行转换
    if (transition.oldState == transition.newState
        && !(transition.oldState.access & (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT))) {
        return;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = transition.oldState.access;
    barrier.dstAccessMask = transition.newState.access;
    barrier.oldLayout = transition.oldState.layout;
    barrier.newLayout = transition.newState.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        commandBuffer,
        transition.oldState.stage ? transition.oldState.stage
            : (transition.oldState.access ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
        transition.newState.stage ? transition.newState.stage
            : (transition.newState.access ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT),
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

} // namespace heisenberg::filtergraph
