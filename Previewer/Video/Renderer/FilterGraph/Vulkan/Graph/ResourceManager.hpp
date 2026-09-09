#pragma once

#include "LogicalResource.hpp"
#include <Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <unordered_map>
#include <vector>
#include <memory>

namespace heisenberg::filtergraph {

class VulkanImageResource;

/// Graph 拥有的资源管理器
class ResourceManager {
public:
    explicit ResourceManager(const VulkanGraphContext& context);
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    /// 分配一个逻辑资源 ID
    LogicalResourceId allocateLogicalResource();

    /// 为逻辑资源分配物理资源
    bool ensurePhysicalResource(LogicalResourceId id, VkExtent2D extent,
                                VkImageUsageFlags usage,
                                const GraphImageContract& contract);

    /// 获取物理资源
    VulkanImageRef getResource(LogicalResourceId id,
                               VulkanSyncPoint ready = {}) const;

    /// 获取资源的当前逻辑状态
    LogicalResourceState getState(LogicalResourceId id) const;

    /// 设置资源的逻辑状态
    void setState(LogicalResourceId id, const LogicalResourceState& state);

    /// 重置所有资源
    void reset();

    /// 清空所有资源
    void clear();

private:
    struct PhysicalResource {
        std::unique_ptr<VulkanImageResource> image;
        LogicalResourceState currentState;
    };

    VulkanGraphContext context_;
    uint32_t nextLogicalId_ = 1;
    std::unordered_map<uint32_t, PhysicalResource> resources_;
};

} // namespace heisenberg::filtergraph
