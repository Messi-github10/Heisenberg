#pragma once

#include "VulkanNode.hpp"

#include <FilterGraph/Interface/ILayerFactory.hpp>

namespace heisenberg::filtergraph {

class VulkanInputLayer final : public VulkanNode, public IInputLayer {
public:
    VulkanInputLayer();

    void setImage(const ImageFormat& format) override;
    void setImage(const VideoFormat& format) override;
    void inputCpuData(uint8_t* data, bool separateRun = false) override;
    void inputCpuData(const VideoFrame& frame, bool separateRun = false) override;
    void inputCpuData(uint8_t* data, const ImageFormat& format,
                      bool separateRun = false) override;
    bool setVulkanInput(const VulkanImageRef& image,
                        int32_t inputIndex = 0) override;

    bool prepare(const VulkanGraphContext& context) override;
    bool beginFrame(const FrameContext& frame) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    ImageFormat declaredFormat_ = {};
    VulkanImageRef externalImage_ = {};
};

class VulkanOutputLayer final : public VulkanNode, public IOutputLayer {
public:
    VulkanOutputLayer();

    void setObserver(IOutputLayerObserver* observer) override;
    bool getVulkanOutput(VulkanImageRef& image,
                         int32_t outputIndex = 0) const override;
    void releaseVulkanOutput(const VulkanImageRef& image,
                             int32_t outputIndex = 0) override;

    bool prepare(const VulkanGraphContext& context) override;
    bool beginFrame(const FrameContext& frame) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    VulkanSyncPoint takeConsumerDone() override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    IOutputLayerObserver* observer_ = nullptr;
    VulkanSyncPoint consumerDone_ = {};
};

} // namespace heisenberg::filtergraph
