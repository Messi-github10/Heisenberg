#pragma once

#include "VulkanImageResource.hpp"
#include "VulkanLayer.hpp"

#include <memory>
#include <string>

namespace heisenberg::filtergraph {

class VulkanComputeLayer : public VulkanLayer {
public:
    ~VulkanComputeLayer() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    explicit VulkanComputeLayer(std::string mark);

    bool configure(const std::vector<ImageFormat>& inputs) override;
    virtual bool supportsFormat(ImageType format) const = 0;
    virtual const char* shaderPath() const = 0;

private:
    bool initializePipeline();
    void destroyPipeline();
    void updateDescriptors(const VulkanImageRef& source,
                           const VulkanImageRef& destination);

    VulkanGraphContext context_ = {};
    std::unique_ptr<VulkanImageResource> outputImage_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
};

class VulkanColorInvertLayer final : public VulkanComputeLayer {
public:
    VulkanColorInvertLayer();

protected:
    bool supportsFormat(ImageType format) const override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
