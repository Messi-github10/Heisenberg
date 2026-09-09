#pragma once

#include <Video/Renderer/FilterGraph/Core/PipeGraph.hpp>
#include "ResourceManager.hpp"
#include "StateTransitionScheduler.hpp"
#include <cstdint>
#include <vector>
#include <memory>

namespace heisenberg::filtergraph {

class VulkanPipeGraph final : public PipeGraph {
public:
    explicit VulkanPipeGraph(const VulkanGraphContext& context);
    ~VulkanPipeGraph() override;

    /// 获取资源管理器（供 Node 使用）
    ResourceManager& resourceManager() { return *resourceManager_; }

protected:
    bool onGraphRebuilt() override;
    bool onRun(const FrameContext& frame) override;
    void onGraphCleared() override;

private:
    bool initialize();
    void shutdown();
    bool waitForPreviousSubmission();
    bool appendWait(std::vector<VulkanSyncPoint>& waits,
                    VulkanSyncPoint wait) const;

    /// 构建状态转换计划
    bool buildTransitionPlan();

    VulkanGraphContext context_;
    VkCommandPool commandPool_       = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_   = VK_NULL_HANDLE;
    VkFence fence_                   = VK_NULL_HANDLE;
    VkSemaphore timeline_            = VK_NULL_HANDLE;
    uint64_t timelineValue_          = 0;
    bool submitted_                  = false;
    bool initialized_                = false;

    // Graph-owned resources
    std::unique_ptr<ResourceManager> resourceManager_;
    std::unique_ptr<StateTransitionScheduler> transitionScheduler_;
};

} // namespace heisenberg::filtergraph
