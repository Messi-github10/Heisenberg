#pragma once

#include "VulkanImageResource.hpp"
#include "VulkanNode.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace heisenberg::filtergraph {

enum class VulkanInputBinding : uint8_t {
    storageImage,
    sampledLinear,
    sampledNearest,
};

class VulkanComputeNode : public VulkanNode {
public:
    ~VulkanComputeNode() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    explicit VulkanComputeNode(std::string mark,
                              int32_t inputCount = 1,
                              int32_t outputCount = 1);

    bool configure(const std::vector<ImageFormat>& inputs) override;

    void setUniformBufferSize(size_t size);
    void updateUniformData(const void* data, size_t size);

    template<typename T>
    void updateUniformData(const T& value) {
        updateUniformData(&value, sizeof(T));
    }

    virtual bool supportsFormat(FormatId format) const;
    virtual bool configureOutputs(const std::vector<ImageFormat>& inputs);
    virtual VulkanInputBinding inputBinding(int32_t inputIndex) const;
    virtual int32_t extraInputCount() const;
    virtual VulkanInputBinding extraInputBinding(int32_t inputIndex) const;
    virtual VulkanImageRef extraInput(int32_t inputIndex) const;
    virtual void setExtraInputLayout(int32_t inputIndex,
                                     VkImageLayout layout);
    virtual VkExtent3D workGroupSize() const;
    virtual const char* shaderPath() const = 0;

private:
    bool initializePipeline();
    bool initializeUniformBuffer();
    bool initializeSamplers();
    bool updateDescriptors();
    bool uploadUniformData();
    void destroyPipeline();
    void destroyUniformBuffer();
    uint32_t findMemoryType(uint32_t bits,
                            VkMemoryPropertyFlags flags) const;
    VkSampler samplerFor(VulkanInputBinding binding) const;

    VulkanGraphContext context_ = {};
    std::vector<std::unique_ptr<VulkanImageResource>> outputImages_;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkSampler nearestSampler_ = VK_NULL_HANDLE;

    VkBuffer uniformBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory uniformMemory_ = VK_NULL_HANDLE;
    void* uniformMapped_ = nullptr;
    VkDeviceSize uniformAllocationSize_ = 0;
    std::vector<uint8_t> uniformData_;
    bool uniformDirty_ = false;
    std::mutex uniformMutex_;
};

} // namespace heisenberg::filtergraph
