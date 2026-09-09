#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace heisenberg::filtergraph {

/// 资源的逻辑状态（在图编译时确定）
struct LogicalResourceState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags access = 0;

    bool operator==(const LogicalResourceState& other) const {
        return layout == other.layout &&
               stage == other.stage &&
               access == other.access;
    }

    bool operator!=(const LogicalResourceState& other) const {
        return !(*this == other);
    }
};

/// 逻辑资源 ID（图中的虚拟资源）
struct LogicalResourceId {
    uint32_t id = 0;

    bool valid() const { return id != 0; }
    bool operator==(const LogicalResourceId& other) const { return id == other.id; }
    bool operator!=(const LogicalResourceId& other) const { return id != other.id; }
};

/// 资源的读写访问模式
enum class ResourceAccessMode {
    Read,           // 只读
    Write,          // 只写
    ReadWrite       // 读写
};

/// Node 对资源的访问描述
struct ResourceAccess {
    LogicalResourceId resource;
    ResourceAccessMode mode = ResourceAccessMode::Read;

    // 访问时期望的状态
    LogicalResourceState expectedState;
};

/// 状态转换记录
struct StateTransition {
    LogicalResourceId resource;
    LogicalResourceState oldState;
    LogicalResourceState newState;

    // 转换在哪个 Node 之前执行
    uint32_t beforeNodeIndex = 0;
};

} // namespace heisenberg::filtergraph
