#pragma once

#include "VulkanComputeNode.hpp"
#include "../VulkanFilterRegistry.hpp"

#include <string>
#include <memory>

namespace heisenberg::filtergraph {

class VulkanManifestComputeNode final : public VulkanComputeNode {
public:
    VulkanManifestComputeNode(const VulkanFilterDescriptor& descriptor,
                              const VulkanGraphParameter& parameter,
                              QJsonObject uniformOverrides = {});
    ~VulkanManifestComputeNode() override;

    bool setExternalInput(int32_t index, const VulkanImageRef& image);

protected:
    bool configureOutputs(const std::vector<ImageFormat>& inputs) override;
    VulkanInputBinding inputBinding(int32_t inputIndex) const override;
    int32_t extraInputCount() const override;
    VulkanInputBinding extraInputBinding(int32_t inputIndex) const override;
    VulkanImageRef extraInput(int32_t inputIndex) const override;
    void setExtraInputLayout(int32_t inputIndex,
                             VkImageLayout layout) override;
    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    const char* shaderPath() const override;

private:
    static QJsonObject parameterObject(const VulkanGraphParameter& parameter);
    void updateUniform(const QJsonObject& parameters);

    VulkanFilterDescriptor descriptor_;
    std::string shaderPath_;
    QJsonObject parameters_;
    VulkanGraphContext context_ = {};
    std::vector<VulkanImageRef> externalInputs_;
    std::unique_ptr<VulkanImageResource> auxiliaryImage_;
    VkBuffer auxiliaryUploadBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory auxiliaryUploadMemory_ = VK_NULL_HANDLE;
    void* auxiliaryUploadMapped_ = nullptr;
    bool auxiliaryUploadPending_ = true;

    bool initializeIdentityAuxiliary(const VulkanGraphContext& context);
    bool initializeAuxiliaryUpload(const VulkanGraphContext& context);
    void uploadIdentityAuxiliary(VkCommandBuffer commandBuffer);
    void destroyAuxiliaryUpload();
    uint32_t findMemoryType(const VulkanGraphContext& context,
                            uint32_t bits,
                            VkMemoryPropertyFlags flags) const;
};

} // namespace heisenberg::filtergraph
