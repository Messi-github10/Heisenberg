#include "VulkanInputAdapter.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Graph/ResourceManager.hpp>
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

std::vector<LogicalResourceRequest>
VulkanInputAdapter::declareResourceRequests() const {
    LogicalResourceRequest request;
    request.kind = LogicalResourceKind::External;
    request.extent = externalImage_.valid() ? externalImage_.extent : VkExtent2D{};
    request.usage = externalImage_.usage;
    request.contract = externalImage_.valid()
        ? externalImage_.contract : kWorkingImageContract;
    request.outputPin = 0;
    request.imported = externalImage_;
    return {request};
}

std::vector<ResourceAccess> VulkanInputAdapter::declareResourceAccess() const {
    // External 资源不经过图内 barrier 管理
    return {};
}

bool VulkanInputAdapter::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanInputAdapter::beginFrame(const FrameContext& frame) {
    if (!externalImage_.valid() || !resourceManager()
        || !resourceManager()->updateImported(
            logicalOutputResource(0), externalImage_)) return false;
    return VulkanNode::beginFrame(frame);
}

void VulkanInputAdapter::record(VkCommandBuffer, const FrameContext&) {}

} // namespace heisenberg::filtergraph
