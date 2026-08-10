#include "VulkanNodes.hpp"

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

VkFormat vkFormatFromImageType(ImageType type) {
    switch (type) {
        case ImageType::r8: return VK_FORMAT_R8_UNORM;
        case ImageType::r16: return VK_FORMAT_R16_UNORM;
        case ImageType::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case ImageType::r32f: return VK_FORMAT_R32_SFLOAT;
        case ImageType::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case ImageType::bgra8: return VK_FORMAT_B8G8R8A8_UNORM;
        case ImageType::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

void transitionImage(VkCommandBuffer commandBuffer, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags sourceStage,
                     VkPipelineStageFlags destinationStage,
                     VkAccessFlags sourceAccess,
                     VkAccessFlags destinationAccess) {
    if (oldLayout == newLayout) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

VulkanInputLayer::VulkanInputLayer()
    : VulkanLayer("VulkanInput", 0, 1) {}

void VulkanInputLayer::setImage(const ImageFormat& format) {
    if (declaredFormat_ == format) return;
    declaredFormat_ = format;
    invalidateGraph();
}

void VulkanInputLayer::setImage(const VideoFormat& format) {
    ImageFormat image;
    image.width = format.width;
    image.height = format.height;
    image.imageType = format.videoType == VideoType::bgra8
        ? ImageType::bgra8 : ImageType::rgba8;
    setImage(image);
}

void VulkanInputLayer::inputCpuData(uint8_t*, bool) {
    LOG_WARN("FilterGraph: VulkanInput only accepts Vulkan images");
}

void VulkanInputLayer::inputCpuData(const VideoFrame&, bool) {
    LOG_WARN("FilterGraph: VulkanInput only accepts Vulkan images");
}

void VulkanInputLayer::inputCpuData(uint8_t*, const ImageFormat&, bool) {
    LOG_WARN("FilterGraph: VulkanInput only accepts Vulkan images");
}

bool VulkanInputLayer::setVulkanInput(
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

bool VulkanInputLayer::configure(const std::vector<ImageFormat>& inputs) {
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

bool VulkanInputLayer::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanInputLayer::beginFrame(const FrameContext& frame) {
    if (!externalImage_.valid()) return false;
    setOutput(0, externalImage_);
    return VulkanLayer::beginFrame(frame);
}

void VulkanInputLayer::record(VkCommandBuffer, const FrameContext&) {}

VulkanOutputLayer::VulkanOutputLayer()
    : VulkanLayer("VulkanOutput", 1, 1) {}

void VulkanOutputLayer::setObserver(IOutputLayerObserver* observer) {
    observer_ = observer;
}

bool VulkanOutputLayer::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || inputs[0].imageType == ImageType::other) return false;
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    if (observer_) observer_->onFormatChanged(inputs[0], 0);
    return true;
}

bool VulkanOutputLayer::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanOutputLayer::beginFrame(const FrameContext& frame) {
    if (!input(0).valid()) return false;
    setOutput(0, input(0));
    return VulkanLayer::beginFrame(frame);
}

void VulkanOutputLayer::record(VkCommandBuffer, const FrameContext&) {}

bool VulkanOutputLayer::getVulkanOutput(
    VulkanImageRef& image, int32_t outputIndex) const {
    if (outputIndex != 0 || !output(0).valid()) return false;
    image = output(0);
    return true;
}

void VulkanOutputLayer::releaseVulkanOutput(
    const VulkanImageRef& image, int32_t outputIndex) {
    if (outputIndex != 0 || !image.valid() || image.image != output(0).image
        || image.generation != output(0).generation) {
        LOG_WARN("FilterGraph: ignored release for an unknown output image");
        return;
    }
    consumerDone_ = image.ready;
}

VulkanSyncPoint VulkanOutputLayer::takeConsumerDone() {
    const VulkanSyncPoint result = consumerDone_;
    consumerDone_ = {};
    return result;
}

VulkanPassthroughLayer::VulkanPassthroughLayer()
    : VulkanLayer("VulkanPassthrough", 1, 1) {}

bool VulkanPassthroughLayer::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || inputs[0].imageType == ImageType::other) return false;
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    return true;
}

bool VulkanPassthroughLayer::prepare(const VulkanGraphContext& context) {
    if (!outputImage_) {
        outputImage_ = std::make_unique<VulkanImageResource>(context);
    }
    const ImageFormat& outputFormat = outputFormats()[0];
    const VkFormat format = vkFormatFromImageType(outputFormat.imageType);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_STORAGE_BIT;
    return outputImage_->ensure(
        {static_cast<uint32_t>(outputFormat.width),
         static_cast<uint32_t>(outputFormat.height)},
        format, usage, kWorkingImageContract);
}

void VulkanPassthroughLayer::record(
    VkCommandBuffer commandBuffer, const FrameContext&) {
    const VulkanImageRef& source = input(0);
    if (!source.valid() || !outputImage_) return;

    const VkImageLayout sourceLayout = source.layout;
    transitionImage(commandBuffer, source.image, sourceLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
    transitionImage(commandBuffer, outputImage_->ref().image,
                    outputImage_->layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    outputImage_->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    outputImage_->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                        ? 0
                        : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {source.extent.width, source.extent.height, 1};
    vkCmdCopyImage(commandBuffer, source.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   outputImage_->ref().image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    transitionImage(commandBuffer, source.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT);
    transitionImage(commandBuffer, outputImage_->ref().image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT);
    outputImage_->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    setOutput(0, outputImage_->ref());
}

} // namespace heisenberg::filtergraph
