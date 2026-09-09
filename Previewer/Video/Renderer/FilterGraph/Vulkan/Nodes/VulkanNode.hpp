#pragma once

#include <Video/Renderer/FilterGraph/Core/BaseNode.hpp>
#include <Video/Renderer/FilterGraph/Vulkan/Graph/LogicalResource.hpp>
#include <vector>

namespace heisenberg::filtergraph {

class VulkanGroupNode;
class ResourceManager;

class VulkanNode : public BaseNode {
public:
    VulkanNode(std::string mark, int32_t inputCount, int32_t outputCount);
    ~VulkanNode() override = default;

    void bindInputs(std::vector<VulkanImageRef> inputs);
    const VulkanImageRef& output(int32_t index) const;

    virtual bool prepare(const VulkanGraphContext& context) = 0;
    virtual bool beginFrame(const FrameContext& frame);
    virtual void record(VkCommandBuffer commandBuffer,
                        const FrameContext& frame) = 0;
    virtual void setCompletion(VulkanSyncPoint completion);
    virtual VulkanSyncPoint takeConsumerDone() { return {}; }
    virtual LogicalResourceId logicalOutputResource(int32_t) const { return {}; }

    // Graph-owned Resource 接口
    /// 声明此 Node 对资源的访问需求
    virtual std::vector<ResourceAccess> declareResourceAccess() const { return {}; }

    /// 从 ResourceManager 分配所需的资源
    virtual bool allocateResources(ResourceManager& manager) { (void)manager; return true; }

    /// 设置 ResourceManager 引用（在 prepare 时由 Graph 设置）
    void setResourceManager(ResourceManager* manager) { resourceManager_ = manager; }
    void bindInputResources(std::vector<LogicalResourceId> resources);

protected:
    const VulkanImageRef& input(int32_t index) const;
    LogicalResourceId inputResource(int32_t index) const;
    void setOutput(int32_t index, const VulkanImageRef& image);
    void bypass();

    /// 获取 ResourceManager（子类可以用来获取资源）
    ResourceManager* resourceManager() const { return resourceManager_; }

private:
    friend class VulkanGroupNode;

    bool configureAsGroupPass(const std::vector<ImageFormat>& inputs) {
        return configure(inputs);
    }

    std::vector<VulkanImageRef> inputs_;
    std::vector<VulkanImageRef> outputs_;
    std::vector<LogicalResourceId> inputResources_;
    ResourceManager* resourceManager_ = nullptr;
};

} // namespace heisenberg::filtergraph
