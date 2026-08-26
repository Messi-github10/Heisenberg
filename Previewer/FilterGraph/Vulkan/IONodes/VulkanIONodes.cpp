#include "VulkanIONodes.hpp"
#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {
namespace {

ImageType imageTypeFromVkFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return ImageType::r8;
        case VK_FORMAT_R16_UNORM: return ImageType::r16;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return ImageType::rgba16f;
        case VK_FORMAT_R32_SFLOAT: return ImageType::r32f;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return ImageType::rgba32f;
        case VK_FORMAT_B8G8R8A8_UNORM: return ImageType::bgra8;
        case VK_FORMAT_R8G8B8A8_UNORM: return ImageType::rgba8;
        default: return ImageType::other;
    }
}

} // namespace

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
    image.imageType = format.format == toFormatId(ImageType::bgra8)
        ? ImageType::bgra8 : ImageType::rgba8;
    setImage(image);
}

bool VulkanInputNode::setVulkanInput(
    const VulkanImageRef& image, int32_t inputIndex) {
    if (inputIndex != 0 || !image.valid()) return false;
    if (image.format != kWorkingImageContract.format
        || image.contract != kWorkingImageContract) {
        LOG_ERROR("FilterGraph: Vulkan input violates the linear BT.2020 "
                  "RGBA16F straight-alpha working contract");
        return false;
    }
    const bool formatChanged = !externalImage_.valid()
        || externalImage_.format != image.format
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
    format.imageType = imageTypeFromVkFormat(externalImage_.format);
    if (format.imageType == ImageType::other) return false;
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
    if (inputs.size() != 1 || inputs[0].imageType == ImageType::other) return false;
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
