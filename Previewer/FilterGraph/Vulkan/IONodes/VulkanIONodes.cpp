#include "VulkanIONodes.hpp"
#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {
VulkanInputNode::VulkanInputNode()
    : VulkanNode("VulkanInput", 0, 1) {}

void VulkanInputNode::setImage(const ImageFormat& format) {
    if (declaredFormat_ == format) return;
    declaredFormat_ = format;
    invalidateGraph();
}

void VulkanInputNode::setImage(const VideoFormat& format) {
    ImageFormat image;
    image.width = format.width;
    image.height = format.height;
    image.format = format.format;
    setImage(image);
}

bool VulkanInputNode::setVulkanInput(
    const VulkanImageRef& image, int32_t inputIndex) {
    if (inputIndex != 0 || !image.valid()) return false;
    if (image.vkFormat != imageFormatToVkFormat(kWorkingImageContract.format)
        || image.contract != kWorkingImageContract) {
        LOG_ERROR("FilterGraph: Vulkan input violates the linear BT.2020 "
                  "RGBA16F straight-alpha working contract");
        return false;
    }
    const bool formatChanged = !externalImage_.valid()
        || externalImage_.vkFormat != image.vkFormat
        || externalImage_.contract.format != image.contract.format
        || externalImage_.extent.width != image.extent.width
        || externalImage_.extent.height != image.extent.height;
    externalImage_ = image;
    if (formatChanged) invalidateGraph();
    return true;
}

bool VulkanInputNode::configure(const std::vector<ImageFormat>& inputs) {
    if (!inputs.empty() || !externalImage_.valid()
        || externalImage_.contract != kWorkingImageContract) {
        return false;
    }
    ImageFormat format;
    format.width = static_cast<int32_t>(externalImage_.extent.width);
    format.height = static_cast<int32_t>(externalImage_.extent.height);
    format.format = externalImage_.contract.format;
    if (format.format == toFormatId(ImageType::none)) return false;
    if (declaredFormat_.width > 0 && declaredFormat_ != format) {
        LOG_ERROR("FilterGraph: external input does not match declared format");
        return false;
    }
    setOutputFormat(0, format);
    return true;
}

bool VulkanInputNode::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanInputNode::beginFrame(const FrameContext& frame) {
    if (!externalImage_.valid()) return false;
    setOutput(0, externalImage_);
    return VulkanNode::beginFrame(frame);
}

void VulkanInputNode::record(VkCommandBuffer, const FrameContext&) {}

VulkanOutputNode::VulkanOutputNode()
    : VulkanNode("VulkanOutput", 1, 1) {}

void VulkanOutputNode::setObserver(IOutputNodeObserver* observer) {
    observer_ = observer;
}

bool VulkanOutputNode::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1
        || inputs[0].format == toFormatId(ImageType::none)) return false;
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    if (observer_) observer_->onFormatChanged(inputs[0], 0);
    return true;
}

bool VulkanOutputNode::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanOutputNode::beginFrame(const FrameContext& frame) {
    if (!input(0).valid()) return false;
    setOutput(0, input(0));
    return VulkanNode::beginFrame(frame);
}

void VulkanOutputNode::record(VkCommandBuffer, const FrameContext&) {}

bool VulkanOutputNode::getVulkanOutput(
    VulkanImageRef& image, int32_t outputIndex) const {
    if (outputIndex != 0 || !output(0).valid()) return false;
    image = output(0);
    return true;
}

void VulkanOutputNode::releaseVulkanOutput(
    const VulkanImageRef& image, int32_t outputIndex) {
    if (outputIndex != 0 || !image.valid() || image.image != output(0).image
        || image.generation != output(0).generation) {
        LOG_WARN("FilterGraph: ignored release for an unknown output image");
        return;
    }
    consumerDone_ = image.ready;
}

VulkanSyncPoint VulkanOutputNode::takeConsumerDone() {
    const VulkanSyncPoint result = consumerDone_;
    consumerDone_ = {};
    return result;
}

} // namespace heisenberg::filtergraph
