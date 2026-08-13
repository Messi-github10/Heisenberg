#pragma once

#include "VulkanNode.hpp"

#include <array>

namespace heisenberg::filtergraph {

class VulkanHistogramNode final : public VulkanNode {
public:
    VulkanHistogramNode();
    ~VulkanHistogramNode() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    void setCompletion(VulkanSyncPoint completion) override;

    bool readBins(std::array<uint32_t, 256>& bins) const;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    bool initializePipeline();
    bool initializeBuffer();
    void destroyResources();
    uint32_t findMemoryType(uint32_t bits,
                            VkMemoryPropertyFlags flags) const;

    VulkanGraphContext context_ = {};
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkBuffer binsBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory binsMemory_ = VK_NULL_HANDLE;
    void* binsMapped_ = nullptr;
    VulkanSyncPoint binsReady_ = {};
};

} // namespace heisenberg::filtergraph
