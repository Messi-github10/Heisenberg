#pragma once

#include "VulkanComputeNode.hpp"
#include "VulkanImageResource.hpp"

#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class VulkanLutNode final : public VulkanComputeNode {
public:
    VulkanLutNode();
    ~VulkanLutNode() override;

    bool setLutImage(const VulkanImageRef& image);
    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    VulkanInputBinding inputBinding(int32_t inputIndex) const override;
    int32_t extraInputCount() const override;
    VulkanInputBinding extraInputBinding(int32_t inputIndex) const override;
    VulkanImageRef extraInput(int32_t inputIndex) const override;
    void setExtraInputLayout(int32_t inputIndex,
                             VkImageLayout layout) override;
    const char* shaderPath() const override;

private:
    bool initializeIdentityLut(const VulkanGraphContext& context);
    bool initializeUploadBuffer(const VulkanGraphContext& context);
    void destroyUploadBuffer();
    void uploadIdentityLut(VkCommandBuffer commandBuffer);
    uint32_t findMemoryType(const VulkanGraphContext& context, uint32_t bits,
                            VkMemoryPropertyFlags flags) const;

    VulkanGraphContext context_ = {};
    std::unique_ptr<VulkanImageResource> lutImage_;
    VulkanImageRef externalLut_ = {};
    VkBuffer uploadBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory_ = VK_NULL_HANDLE;
    void* uploadMapped_ = nullptr;
    bool uploadPending_ = true;
};

} // namespace heisenberg::filtergraph
