#pragma once

#include "../VulkanFilterRegistry.hpp"
#include "VulkanNode.hpp"

#include <cstddef>

namespace heisenberg::filtergraph {

class VulkanReadbackNode final : public VulkanNode {
public:
    explicit VulkanReadbackNode(const VulkanFilterDescriptor& descriptor);
    ~VulkanReadbackNode() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    void setCompletion(VulkanSyncPoint completion) override;

    bool readback(void* destination, size_t size) const;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    bool initializeBuffer();
    bool initializePipeline();
    void destroyResources();
    uint32_t findMemoryType(uint32_t bits,
                            VkMemoryPropertyFlags flags) const;

    VulkanFilterDescriptor descriptor_;
    VulkanGraphContext context_ = {};
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    void* readbackMapped_ = nullptr;
    VulkanSyncPoint readbackReady_ = {};
};

} // namespace heisenberg::filtergraph
