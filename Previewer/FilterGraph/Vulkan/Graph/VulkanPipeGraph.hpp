#pragma once

#include <FilterGraph/Core/PipeGraph.hpp>
#include <cstdint>
#include <vector>

namespace heisenberg::filtergraph {

class VulkanPipeGraph final : public PipeGraph {
public:
    explicit VulkanPipeGraph(const VulkanGraphContext& context);
    ~VulkanPipeGraph() override;

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

    VulkanGraphContext context_;
    VkCommandPool commandPool_       = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_   = VK_NULL_HANDLE;
    VkFence fence_                   = VK_NULL_HANDLE;
    VkSemaphore timeline_            = VK_NULL_HANDLE;
    uint64_t timelineValue_          = 0;
    bool submitted_                  = false;
    bool initialized_                = false;
};

} // namespace heisenberg::filtergraph
