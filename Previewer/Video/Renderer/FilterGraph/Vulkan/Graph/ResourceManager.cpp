#include "ResourceManager.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Resources/VulkanImageResource.hpp>
#include <Utiles/Logger.hpp>

namespace heisenberg::filtergraph {

ResourceManager::ResourceManager(const VulkanGraphContext& context)
    : context_(context) {
}

ResourceManager::~ResourceManager() {
    clear();
}

LogicalResourceId ResourceManager::allocateLogicalResource() {
    LogicalResourceId id;
    id.id = nextLogicalId_++;
    return id;
}

bool ResourceManager::ensurePhysicalResource(
    LogicalResourceId id, VkExtent2D extent,
    VkImageUsageFlags usage, const GraphImageContract& contract) {

    if (!id.valid()) {
        LOG_ERROR("ResourceManager: invalid logical resource ID");
        return false;
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) {
        // 创建新的物理资源
        PhysicalResource physical;
        physical.image = std::make_unique<VulkanImageResource>(context_);
        physical.currentState.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        physical.currentState.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        physical.currentState.access = 0;

        if (!physical.image->ensure(extent, usage, contract)) {
            LOG_ERROR("ResourceManager: failed to create physical resource");
            return false;
        }

        resources_[id.id] = std::move(physical);
    } else {
        // 复用现有资源
        if (!it->second.image->ensure(extent, usage, contract)) {
            LOG_ERROR("ResourceManager: failed to ensure physical resource");
            return false;
        }
    }

    return true;
}

VulkanImageRef ResourceManager::getResource(LogicalResourceId id,
                                            VulkanSyncPoint ready) const {
    if (!id.valid()) {
        return VulkanImageRef{};
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) {
        return VulkanImageRef{};
    }

    return it->second.image->ref(ready);
}

LogicalResourceState ResourceManager::getState(LogicalResourceId id) const {
    if (!id.valid()) {
        return LogicalResourceState{};
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) {
        return LogicalResourceState{};
    }

    return it->second.currentState;
}

void ResourceManager::setState(LogicalResourceId id,
                               const LogicalResourceState& state) {
    if (!id.valid()) {
        return;
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) {
        return;
    }

    it->second.currentState = state;
    it->second.image->setLayout(state.layout);
}

void ResourceManager::reset() {
    for (auto& pair : resources_) {
        pair.second.image->reset();
        pair.second.currentState.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        pair.second.currentState.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        pair.second.currentState.access = 0;
    }
}

void ResourceManager::clear() {
    resources_.clear();
    nextLogicalId_ = 1;
}

} // namespace heisenberg::filtergraph
