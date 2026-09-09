#pragma once

#include "LogicalResource.hpp"
#include <vector>
#include <unordered_map>

namespace heisenberg::filtergraph {

/// 状态转换管理器
/// 在图编译时构建状态转换计划，在执行时插入 barrier
class StateTransitionScheduler {
public:
    StateTransitionScheduler() = default;

    /// 记录一个 Node 对资源的访问
    /// @param nodeIndex Node 在执行序列中的索引
    /// @param access 访问描述
    /// @param currentState 资源当前的状态
    /// @return 如果需要状态转换，返回转换描述
    StateTransition recordAccess(uint32_t nodeIndex,
                                 const ResourceAccess& access,
                                 const LogicalResourceState& currentState);

    /// 获取在指定 Node 之前需要执行的所有状态转换
    const std::vector<StateTransition>& getTransitionsBefore(uint32_t nodeIndex) const;

    /// 清空所有记录
    void clear();

    /// 执行状态转换（插入 barrier）
    static void executeTransition(VkCommandBuffer commandBuffer,
                                   VkImage image,
                                   const StateTransition& transition);

private:
    // nodeIndex -> transitions
    std::unordered_map<uint32_t, std::vector<StateTransition>> transitions_;
    static std::vector<StateTransition> emptyTransitions_;
};

} // namespace heisenberg::filtergraph
