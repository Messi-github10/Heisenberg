#pragma once

#include <FilterGraph/Common/FilterCommon.hpp>

namespace heisenberg::filtergraph {

class VulkanImageResource {
public:
    explicit VulkanImageResource(const VulkanGraphContext& context);
    ~VulkanImageResource();

    VulkanImageResource(const VulkanImageResource&) = delete;
    VulkanImageResource& operator=(const VulkanImageResource&) = delete;

    bool ensure(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage);
    void reset();
    VulkanImageRef ref(VulkanSyncPoint ready = {}) const;

    VkImageLayout layout() const { return layout_; }
    void setLayout(VkImageLayout layout) { layout_ = layout; }

private:
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;

    VulkanGraphContext context_;
    VkImage image_             = VK_NULL_HANDLE;
    VkImageView view_          = VK_NULL_HANDLE;
    VkDeviceMemory memory_     = VK_NULL_HANDLE;
    VkExtent2D extent_         = {};
    VkFormat format_           = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage_   = 0;
    VkImageLayout layout_      = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t generation_       = 0;
};

} // namespace heisenberg::filtergraph
