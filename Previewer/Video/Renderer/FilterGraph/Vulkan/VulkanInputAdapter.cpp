#include "VulkanInputAdapter.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {

VulkanInputAdapter::VulkanInputAdapter()
    : VulkanNode("VulkanInput", 0, 1) {}

void VulkanInputAdapter::setImage(const ImageFormat& format) {
    if (declaredFormat_ == format) return;
    declaredFormat_ = format;
    invalidateGraph();
}

void VulkanInputAdapter::setImage(const VideoFormat& format) {
    ImageFormat image;
    image.width = format.width;
    image.height = format.height;
    image.format = format.format;
    setImage(image);
}

bool VulkanInputAdapter::setVulkanInput(
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

bool VulkanInputAdapter::configure(const std::vector<ImageFormat>& inputs) {
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

bool VulkanInputAdapter::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanInputAdapter::beginFrame(const FrameContext& frame) {
    if (!externalImage_.valid()) return false;
    setOutput(0, externalImage_);
    return VulkanNode::beginFrame(frame);
}

void VulkanInputAdapter::record(VkCommandBuffer, const FrameContext&) {}

} // namespace heisenberg::filtergraph
