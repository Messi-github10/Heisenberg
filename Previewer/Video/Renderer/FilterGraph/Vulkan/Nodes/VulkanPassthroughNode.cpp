#include "VulkanPassthroughNode.hpp"
#include <volk.h>

namespace heisenberg::filtergraph {
namespace {

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

VulkanPassthroughNode::VulkanPassthroughNode()
    : VulkanNode("VulkanPassthrough", 1, 1) {}

bool VulkanPassthroughNode::configure(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1
        || inputs[0].format == toFormatId(ImageType::none)) {
        return false;
    }
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    return true;
}

bool VulkanPassthroughNode::prepare(const VulkanGraphContext& context) {
    if (!outputImage_) {
        outputImage_ = std::make_unique<VulkanImageResource>(context);
    }
    const ImageFormat& outputFormat = outputFormats()[0];
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_STORAGE_BIT;
    GraphImageContract contract = kWorkingImageContract;
    contract.format = outputFormat.format;
    return outputImage_->ensure(
        {static_cast<uint32_t>(outputFormat.width),
         static_cast<uint32_t>(outputFormat.height)},
        usage, contract);
}

void VulkanPassthroughNode::record(
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
