#include "VulkanPassthroughNode.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Graph/ResourceManager.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {

VulkanPassthroughNode::VulkanPassthroughNode()
    : VulkanNode("VulkanPassthrough", 1, 1) {}

bool VulkanPassthroughNode::configure(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1
        || inputs[0].format == toFormatId(ImageType::none)) {
        return false;
    }
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    return true;
}

bool VulkanPassthroughNode::prepare(const VulkanGraphContext& context) {
    context_ = context;

    const ImageFormat& outputFormat = outputFormats()[0];
    outputExtent_ = {
        static_cast<uint32_t>(outputFormat.width),
        static_cast<uint32_t>(outputFormat.height)
    };

    return true;
}

std::vector<ResourceAccess> VulkanPassthroughNode::declareResourceAccess() const {
    std::vector<ResourceAccess> accesses;

    if (inputCount() > 0 && inputResource(0).valid()) {
        ResourceAccess inputAccess;
        inputAccess.resource = inputResource(0);
        inputAccess.mode = ResourceAccessMode::Read;
        inputAccess.expectedState.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        inputAccess.expectedState.stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        inputAccess.expectedState.access = VK_ACCESS_TRANSFER_READ_BIT;
        accesses.push_back(inputAccess);
    }

    // 输入资源：读取（假设输入来自前一个节点，需要在 TRANSFER_SRC 状态）
    // 注意：输入资源的 LogicalResourceId 通常由前一个节点提供
    // 这里我们声明期望的状态
    // 实际的输入资源追踪由 Graph 在构建转换计划时处理

    // 输出资源：写入
    if (outputResource_.valid()) {
        ResourceAccess outputAccess;
        outputAccess.resource = outputResource_;
        outputAccess.mode = ResourceAccessMode::Write;
        outputAccess.expectedState.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        outputAccess.expectedState.stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        outputAccess.expectedState.access = VK_ACCESS_TRANSFER_WRITE_BIT;
        accesses.push_back(outputAccess);
    }

    return accesses;
}

LogicalResourceId VulkanPassthroughNode::logicalOutputResource(int32_t index) const {
    return index == 0 ? outputResource_ : LogicalResourceId{};
}

bool VulkanPassthroughNode::allocateResources(ResourceManager& manager) {
    // 分配输出资源
    outputResource_ = manager.allocateLogicalResource();

    const ImageFormat& outputFormat = outputFormats()[0];
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_STORAGE_BIT;

    GraphImageContract contract = kWorkingImageContract;
    contract.format = outputFormat.format;

    return manager.ensurePhysicalResource(outputResource_, outputExtent_, usage, contract);
}

void VulkanPassthroughNode::record(
    VkCommandBuffer commandBuffer, const FrameContext&) {
    const VulkanImageRef& source = input(0);
    if (!source.valid() || !outputResource_.valid()) return;

    // 从 ResourceManager 获取输出资源
    if (!resourceManager()) return;
    VulkanImageRef outputRef = resourceManager()->getResource(outputResource_);
    if (!outputRef.valid()) return;

    // 注意：输入和输出的状态转换已经由 Graph 自动处理
    // 我们只需要执行实际的拷贝操作

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {source.extent.width, source.extent.height, 1};

    vkCmdCopyImage(commandBuffer, source.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   outputRef.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // 设置输出
    setOutput(0, outputRef);
}

} // namespace heisenberg::filtergraph
