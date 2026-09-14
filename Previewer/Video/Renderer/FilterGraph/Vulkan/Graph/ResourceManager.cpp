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

void ResourceManager::beginRebuild() {
    importedRefs_.clear();
    importedStates_.clear();
    for (const auto& pair : descs_) stale_[pair.first] = true;
}

void ResourceManager::endRebuild() {
    for (auto it = descs_.begin(); it != descs_.end();) {
        if (!stale_[it->first]) {
            ++it;
            continue;
        }
        const uint32_t id = it->first;
        resources_.erase(id);
        importedRefs_.erase(id);
        importedStates_.erase(id);
        stale_.erase(id);
        for (auto key = keyToId_.begin(); key != keyToId_.end();) {
            if (key->second == id) key = keyToId_.erase(key);
            else ++key;
        }
        it = descs_.erase(it);
    }
}

LogicalResourceId ResourceManager::findOrAllocateLogicalResource(
    const std::string& key, const LogicalResourceDesc& desc) {
    auto found = keyToId_.find(key);
    if (found == keyToId_.end()) {
        const LogicalResourceId id{nextLogicalId_++};
        keyToId_[key] = id.id;
        descs_[id.id] = desc;
        stale_[id.id] = false;
        if (desc.kind == LogicalResourceKind::GraphCreated
            && !ensurePhysicalResource(id, desc.extent, desc.usage, desc.contract)) {
            keyToId_.erase(key);
            descs_.erase(id.id);
            stale_.erase(id.id);
            return {};
        }
        return id;
    }

    const LogicalResourceId id{found->second};
    LogicalResourceDesc& existing = descs_.at(id.id);
    stale_[id.id] = false;
    const bool kindChanged = existing.kind != desc.kind;
    const bool shapeChanged = existing.extent.width != desc.extent.width
        || existing.extent.height != desc.extent.height
        || existing.usage != desc.usage
        || existing.contract != desc.contract;
    existing = desc;
    if (desc.kind == LogicalResourceKind::GraphCreated) {
        if (kindChanged) {
            importedRefs_.erase(id.id);
            importedStates_.erase(id.id);
        }
        if ((kindChanged || shapeChanged || resources_.find(id.id) == resources_.end())
            && !ensurePhysicalResource(id, desc.extent, desc.usage, desc.contract)) {
            return {};
        }
    } else {
        if (kindChanged) resources_.erase(id.id);
        importedRefs_.erase(id.id);
        importedStates_.erase(id.id);
    }
    return id;
}

bool ResourceManager::updateImported(LogicalResourceId id,
                                     const VulkanImageRef& ref) {
    if (!id.valid() || !ref.valid()) return false;
    auto desc = descs_.find(id.id);
    if (desc == descs_.end() || desc->second.kind != LogicalResourceKind::External) {
        return false;
    }
    importedRefs_[id.id] = ref;
    LogicalResourceState state;
    state.layout = ref.layout;
    state.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    state.access = 0;
    importedStates_[id.id] = state;
    desc->second.extent = ref.extent;
    desc->second.usage = ref.usage;
    desc->second.contract = ref.contract;
    return true;
}

const LogicalResourceDesc* ResourceManager::description(LogicalResourceId id) const {
    auto found = descs_.find(id.id);
    return found == descs_.end() ? nullptr : &found->second;
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
        it->second.currentState = {};
        it->second.ready = {};
    }

    return true;
}

VulkanImageRef ResourceManager::getResource(LogicalResourceId id,
                                            VulkanSyncPoint ready) const {
    if (!id.valid()) {
        return VulkanImageRef{};
    }

    auto desc = descs_.find(id.id);
    if (desc != descs_.end()
        && desc->second.kind == LogicalResourceKind::External) {
        auto imported = importedRefs_.find(id.id);
        if (imported == importedRefs_.end()) return {};
        VulkanImageRef result = imported->second;
        if (ready.valid()) result.ready = ready;
        return result;
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) {
        return VulkanImageRef{};
    }

    VulkanImageRef result = it->second.image->ref(ready);
    if (!ready.valid()) result.ready = it->second.ready;
    return result;
}

void ResourceManager::setReady(LogicalResourceId id, VulkanSyncPoint ready) {
    auto physical = resources_.find(id.id);
    if (physical != resources_.end()) physical->second.ready = ready;
}

LogicalResourceState ResourceManager::getState(LogicalResourceId id) const {
    if (!id.valid()) {
        return LogicalResourceState{};
    }

    auto desc = descs_.find(id.id);
    if (desc != descs_.end()
        && desc->second.kind == LogicalResourceKind::External) {
        auto state = importedStates_.find(id.id);
        return state == importedStates_.end() ? LogicalResourceState{}
                                               : state->second;
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

    auto desc = descs_.find(id.id);
    if (desc != descs_.end()
        && desc->second.kind == LogicalResourceKind::External) {
        importedStates_[id.id] = state;
        auto imported = importedRefs_.find(id.id);
        if (imported != importedRefs_.end()) imported->second.layout = state.layout;
        return;
    }

    auto it = resources_.find(id.id);
    if (it == resources_.end()) return;
    it->second.currentState = state;
    it->second.image->setLayout(state.layout);
}

void ResourceManager::clear() {
    resources_.clear();
    keyToId_.clear();
    descs_.clear();
    stale_.clear();
    importedRefs_.clear();
    importedStates_.clear();
    nextLogicalId_ = 1;
}

} // namespace heisenberg::filtergraph
