#pragma once

#include "LogicalResource.hpp"
#include <Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>

namespace heisenberg::filtergraph {

class VulkanImageResource;
class VulkanPipeGraph;

/// Graph 拥有的资源管理器
class ResourceManager {
public:
    explicit ResourceManager(const VulkanGraphContext& context);
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    void beginRebuild();
    void endRebuild();
    bool updateImported(LogicalResourceId id, const VulkanImageRef& ref);
    const LogicalResourceDesc* description(LogicalResourceId id) const;

    VulkanImageRef getResource(LogicalResourceId id,
                               VulkanSyncPoint ready = {}) const;
    void setReady(LogicalResourceId id, VulkanSyncPoint ready);
    LogicalResourceState getState(LogicalResourceId id) const;
    void setState(LogicalResourceId id, const LogicalResourceState& state);
    void clear();

private:
    friend class VulkanPipeGraph;

    LogicalResourceId findOrAllocateLogicalResource(
        const std::string& key, const LogicalResourceDesc& desc);
    bool ensurePhysicalResource(LogicalResourceId id, VkExtent2D extent,
                                VkImageUsageFlags usage,
                                const GraphImageContract& contract);

    struct PhysicalResource {
        std::unique_ptr<VulkanImageResource> image;
        LogicalResourceState currentState;
        VulkanSyncPoint ready;
    };

    VulkanGraphContext context_;
    uint32_t nextLogicalId_ = 1;
    std::unordered_map<uint32_t, PhysicalResource> resources_;
    std::unordered_map<std::string, uint32_t> keyToId_;
    std::unordered_map<uint32_t, LogicalResourceDesc> descs_;
    std::unordered_map<uint32_t, bool> stale_;
    std::unordered_map<uint32_t, VulkanImageRef> importedRefs_;
    std::unordered_map<uint32_t, LogicalResourceState> importedStates_;
};

} // namespace heisenberg::filtergraph
