#pragma once

#include "VulkanNode.hpp"
#include <MultiMedia/Video/Renderer/FilterGraph/Common/GpuProgram.hpp>
#include <memory>

namespace heisenberg::filtergraph {

// Records only; submission and synchronization belong to VulkanPipeGraph.
class VulkanProgramNode final : public VulkanNode {
public:
    VulkanProgramNode(std::shared_ptr<const GpuProgram> program,
                      GraphImageContract input, GraphImageContract output);
    ~VulkanProgramNode() override;
    // Only UBO values/provider may differ for an existing immutable identity.
    bool setProgram(std::shared_ptr<const GpuProgram> program);
    bool beginFrame(const FrameContext& frame) override;
    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer command, const FrameContext& frame) override;
    void setCompletion(VulkanSyncPoint completion) override;
    std::vector<LogicalResourceRequest> declareResourceRequests() const override;
    std::vector<ResourceAccess> declareResourceAccess() const override;
protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg::filtergraph
