#pragma once

#include <Video/Renderer/FilterGraph/Core/BaseNode.hpp>
#include <Video/Renderer/FilterGraph/Vulkan/Graph/LogicalResource.hpp>
#include <string>
#include <vector>

namespace heisenberg::filtergraph {

class VulkanGroupNode;
class ResourceManager;

class VulkanNode : public BaseNode {
public:
    VulkanNode(std::string mark, int32_t inputCount, int32_t outputCount);
    ~VulkanNode() override = default;

    VulkanImageRef input(int32_t index) const;
    VulkanImageRef output(int32_t index) const;

    virtual bool prepare(const VulkanGraphContext& context) = 0;
    virtual bool beginFrame(const FrameContext& frame);
    virtual void record(VkCommandBuffer commandBuffer,
                        const FrameContext& frame) = 0;
    virtual void setCompletion(VulkanSyncPoint completion);
    virtual VulkanSyncPoint takeConsumerDone() { return {}; }
    virtual LogicalResourceId logicalOutputResource(int32_t index) const;

    virtual std::vector<LogicalResourceRequest> declareResourceRequests() const {
        return {};
    }
    virtual std::vector<ResourceAccess> declareResourceAccess() const { return {}; }
    virtual void bindDeclaredResources(
        const std::vector<LogicalResourceId>& resources);

    void setResourceManager(ResourceManager* manager) { resourceManager_ = manager; }
    void bindInputResources(std::vector<LogicalResourceId> resources);

protected:
    LogicalResourceId inputResource(int32_t index) const;

    /// 获取 ResourceManager（子类可以用来获取资源）
    ResourceManager* resourceManager() const { return resourceManager_; }

private:
    friend class VulkanGroupNode;

    bool configureAsGroupPass(const std::vector<ImageFormat>& inputs) {
        return configure(inputs);
    }

    std::vector<LogicalResourceId> inputResources_;
    std::vector<LogicalResourceId> outputResources_;
    ResourceManager* resourceManager_ = nullptr;
};

} // namespace heisenberg::filtergraph
