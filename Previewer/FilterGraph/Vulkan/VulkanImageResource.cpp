#include "VulkanImageResource.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

#include <limits>

namespace heisenberg::filtergraph {

VulkanImageResource::VulkanImageResource(const VulkanGraphContext& context)
    : context_(context) {}

VulkanImageResource::~VulkanImageResource() {
    reset();
}

uint32_t VulkanImageResource::findMemoryType(
    uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i))
            && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

bool VulkanImageResource::ensure(
    VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
    const GraphImageContract& contract) {
    if (image_ && extent_.width == extent.width && extent_.height == extent.height
        && format_ == format && usage_ == usage) {
        contract_ = contract;
        return true;
    }

    reset();
    if (!context_.device || extent.width == 0 || extent.height == 0
        || format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(context_.device, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context_.device, image_, &requirements);
    const uint32_t memoryType = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) {
        LOG_ERROR("FilterGraph: no device-local image memory type");
        reset();
        return false;
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(context_.device, &allocation, nullptr, &memory_) != VK_SUCCESS
        || vkBindImageMemory(context_.device, image_, memory_, 0) != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: image memory allocation failed");
        reset();
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(context_.device, &viewInfo, nullptr, &view_) != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: vkCreateImageView failed");
        reset();
        return false;
    }

    extent_ = extent;
    format_ = format;
    usage_ = usage;
    contract_ = contract;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ++generation_;
    return true;
}

void VulkanImageResource::reset() {
    if (!context_.device) return;
    if (view_) vkDestroyImageView(context_.device, view_, nullptr);
    if (image_) vkDestroyImage(context_.device, image_, nullptr);
    if (memory_) vkFreeMemory(context_.device, memory_, nullptr);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    extent_ = {};
    format_ = VK_FORMAT_UNDEFINED;
    usage_ = 0;
    contract_ = {};
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

VulkanImageRef VulkanImageResource::ref(VulkanSyncPoint ready) const {
    VulkanImageRef result;
    result.image = image_;
    result.view = view_;
    result.format = format_;
    result.extent = extent_;
    result.usage = usage_;
    result.layout = layout_;
    result.queueFamilyIndex = context_.queueFamilyIndex;
    result.ready = ready;
    result.generation = generation_;
    result.contract = contract_;
    return result;
}

} // namespace heisenberg::filtergraph
