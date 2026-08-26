#include "VulkanLutNode.hpp"

#include <volk.h>

#include <array>
#include <bit>
#include <cstring>
#include <limits>

#ifndef HEISENBERG_LUT_SHADER_PATH
#error HEISENBERG_LUT_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {
namespace {

constexpr uint32_t kLutCubeSize = 64;
constexpr uint32_t kLutTileSize = 64;
constexpr uint32_t kLutImageSize = kLutCubeSize * 8;

uint16_t floatToHalf(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 0x3c00;
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = ((bits >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = (bits >> 13) & 0x3ff;
    return static_cast<uint16_t>((exponent << 10) | mantissa);
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

VulkanLutNode::VulkanLutNode()
    : VulkanComputeNode("VulkanLut") {}

VulkanLutNode::~VulkanLutNode() {
    destroyUploadBuffer();
}

bool VulkanLutNode::setLutImage(const VulkanImageRef& image) {
    if (!image.valid() || !image.view
        || image.contract != kWorkingImageContract
        || image.extent.width != kLutImageSize
        || image.extent.height != kLutImageSize
        || !(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    externalLut_ = image;
    return true;
}

uint32_t VulkanLutNode::findMemoryType(
    const VulkanGraphContext& context, uint32_t bits,
    VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index))
            && (properties.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

bool VulkanLutNode::initializeUploadBuffer(const VulkanGraphContext& context) {
    constexpr VkDeviceSize size =
        static_cast<VkDeviceSize>(kLutImageSize) * kLutImageSize * 4
        * sizeof(uint16_t);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context.device, &bufferInfo, nullptr, &uploadBuffer_)
        != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, uploadBuffer_, &requirements);
    const uint32_t memoryType = findMemoryType(
        context, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(context.device, &allocation, nullptr, &uploadMemory_)
            != VK_SUCCESS
        || vkBindBufferMemory(context.device, uploadBuffer_, uploadMemory_, 0)
            != VK_SUCCESS
        || vkMapMemory(context.device, uploadMemory_, 0, size, 0, &uploadMapped_)
            != VK_SUCCESS) {
        return false;
    }

    auto* pixels = static_cast<uint16_t*>(uploadMapped_);
    for (uint32_t blue = 0; blue < kLutCubeSize; ++blue) {
        const uint32_t tileX = blue % 8;
        const uint32_t tileY = blue / 8;
        for (uint32_t green = 0; green < kLutCubeSize; ++green) {
            for (uint32_t red = 0; red < kLutCubeSize; ++red) {
                const uint32_t x = tileX * kLutTileSize + red;
                const uint32_t y = tileY * kLutTileSize + green;
                const size_t index = (static_cast<size_t>(y) * kLutImageSize + x) * 4;
                pixels[index] = floatToHalf(static_cast<float>(red) / 63.0f);
                pixels[index + 1] = floatToHalf(static_cast<float>(green) / 63.0f);
                pixels[index + 2] = floatToHalf(static_cast<float>(blue) / 63.0f);
                pixels[index + 3] = 0x3c00;
            }
        }
    }
    return true;
}

bool VulkanLutNode::initializeIdentityLut(const VulkanGraphContext& context) {
    lutImage_ = std::make_unique<VulkanImageResource>(context);
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return lutImage_->ensure({kLutImageSize, kLutImageSize}, usage,
                             kWorkingImageContract)
        && initializeUploadBuffer(context);
}

bool VulkanLutNode::prepare(const VulkanGraphContext& context) {
    if (context_.device && context_.device != context.device) {
        destroyUploadBuffer();
        lutImage_.reset();
        externalLut_ = {};
        uploadPending_ = true;
    }
    context_ = context;
    if (!externalLut_.valid() && !lutImage_
        && !initializeIdentityLut(context)) {
        return false;
    }
    return VulkanComputeNode::prepare(context);
}

VulkanInputBinding VulkanLutNode::inputBinding(int32_t) const {
    return VulkanInputBinding::storageImage;
}

int32_t VulkanLutNode::extraInputCount() const {
    return 1;
}

VulkanInputBinding VulkanLutNode::extraInputBinding(int32_t) const {
    return VulkanInputBinding::sampledLinear;
}

VulkanImageRef VulkanLutNode::extraInput(int32_t) const {
    if (externalLut_.valid()) return externalLut_;
    return lutImage_ ? lutImage_->ref() : VulkanImageRef{};
}

void VulkanLutNode::setExtraInputLayout(int32_t, VkImageLayout layout) {
    if (externalLut_.valid()) {
        externalLut_.layout = layout;
    } else if (lutImage_) {
        lutImage_->setLayout(layout);
    }
}

void VulkanLutNode::uploadIdentityLut(VkCommandBuffer commandBuffer) {
    if (!uploadPending_ || !lutImage_) return;
    const VulkanImageRef image = lutImage_->ref();
    transitionImage(commandBuffer, image.image, lutImage_->layout(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    lutImage_->layout() == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kLutImageSize, kLutImageSize, 1};
    vkCmdCopyBufferToImage(commandBuffer, uploadBuffer_, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    lutImage_->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    uploadPending_ = false;
}

void VulkanLutNode::record(VkCommandBuffer commandBuffer,
                           const FrameContext& frame) {
    uploadIdentityLut(commandBuffer);
    VulkanComputeNode::record(commandBuffer, frame);
}

void VulkanLutNode::destroyUploadBuffer() {
    if (!context_.device) return;
    if (uploadMapped_ && uploadMemory_) {
        vkUnmapMemory(context_.device, uploadMemory_);
    }
    if (uploadBuffer_) vkDestroyBuffer(context_.device, uploadBuffer_, nullptr);
    if (uploadMemory_) vkFreeMemory(context_.device, uploadMemory_, nullptr);
    uploadMapped_ = nullptr;
    uploadBuffer_ = VK_NULL_HANDLE;
    uploadMemory_ = VK_NULL_HANDLE;
}

const char* VulkanLutNode::shaderPath() const {
    return HEISENBERG_LUT_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
