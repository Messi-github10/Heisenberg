#include "VulkanReadbackNode.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#ifndef HEISENBERG_FILTER_SHADER_BINARY_DIR
#define HEISENBERG_FILTER_SHADER_BINARY_DIR "."
#endif

namespace heisenberg::filtergraph {
namespace {

std::vector<uint32_t> loadShaderCode(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("FilterGraph: failed to open shader '{}'", path);
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % static_cast<std::streamsize>(sizeof(uint32_t)) != 0) {
        return {};
    }
    std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) return {};
    return code;
}

void transitionImage(VkCommandBuffer commandBuffer, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags sourceStage,
                     VkPipelineStageFlags destinationStage,
                     VkAccessFlags sourceAccess,
                     VkAccessFlags destinationAccess) {
    if (oldLayout == newLayout) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

VulkanReadbackNode::VulkanReadbackNode(
    const VulkanFilterDescriptor& descriptor)
    : VulkanNode(descriptor.displayName, descriptor.inputCount,
                 descriptor.outputCount),
      descriptor_(descriptor) {}

VulkanReadbackNode::~VulkanReadbackNode() {
    destroyResources();
}

bool VulkanReadbackNode::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != static_cast<size_t>(inputCount())
        || inputs.empty()) {
        return false;
    }
    for (int32_t index = 0; index < inputCount(); ++index) {
        if (inputs[static_cast<size_t>(index)].format
            != kWorkingImageContract.format) {
            return false;
        }
        setInputFormat(index, inputs[static_cast<size_t>(index)]);
    }
    if (descriptor_.passthroughOutput && outputCount() == inputCount()) {
        for (int32_t index = 0; index < outputCount(); ++index) {
            setOutputFormat(index, inputs[static_cast<size_t>(index)]);
        }
    }
    return true;
}

uint32_t VulkanReadbackNode::findMemoryType(
    uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index))
            && (properties.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

bool VulkanReadbackNode::initializeBuffer() {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = static_cast<VkDeviceSize>(descriptor_.readbackSize);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context_.device, &bufferInfo, nullptr, &readbackBuffer_)
        != VK_SUCCESS) return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context_.device, readbackBuffer_, &requirements);
    const uint32_t memoryType = findMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    return vkAllocateMemory(context_.device, &allocation, nullptr, &readbackMemory_)
            == VK_SUCCESS
        && vkBindBufferMemory(context_.device, readbackBuffer_, readbackMemory_, 0)
            == VK_SUCCESS
        && vkMapMemory(context_.device, readbackMemory_, 0,
                       static_cast<VkDeviceSize>(descriptor_.readbackSize), 0,
                       &readbackMapped_) == VK_SUCCESS;
}

bool VulkanReadbackNode::initializePipeline() {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(static_cast<size_t>(inputCount()) + 1);
    for (int32_t index = 0; index < inputCount(); ++index) {
        bindings.push_back({static_cast<uint32_t>(index),
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    bindings.push_back({descriptor_.readbackBinding,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr});

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(context_.device, &layoutInfo, nullptr,
                                    &descriptorSetLayout_) != VK_SUCCESS) {
        return false;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkCreatePipelineLayout(context_.device, &pipelineLayoutInfo, nullptr,
                               &pipelineLayout_) != VK_SUCCESS) return false;

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<uint32_t>(inputCount())},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    }};
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(context_.device, &poolInfo, nullptr,
                               &descriptorPool_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(context_.device, &setInfo, &descriptorSet_)
        != VK_SUCCESS) return false;

    const std::string shaderPath = std::string(HEISENBERG_FILTER_SHADER_BINARY_DIR)
        + "/" + descriptor_.shaderBinary;
    const std::vector<uint32_t> code = loadShaderCode(shaderPath.c_str());
    if (code.empty()) return false;
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = code.size() * sizeof(uint32_t);
    shaderInfo.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context_.device, &shaderInfo, nullptr, &shader)
        != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stageInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shader;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout_;
    const VkResult result = vkCreateComputePipelines(
        context_.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(context_.device, shader, nullptr);
    return result == VK_SUCCESS;
}

bool VulkanReadbackNode::prepare(const VulkanGraphContext& context) {
    if (!context.device || !context.physicalDevice
        || descriptor_.readbackSize == 0) return false;
    if (context_.device && context_.device != context.device) destroyResources();
    context_ = context;
    if (pipeline_) return true;
    if (!initializeBuffer() || !initializePipeline()) {
        destroyResources();
        return false;
    }
    return true;
}

void VulkanReadbackNode::record(VkCommandBuffer commandBuffer,
                                const FrameContext&) {
    if (!pipeline_ || !readbackBuffer_) return;
    const VulkanImageRef source = input(0);
    if (!source.valid() || !source.view
        || source.contract != kWorkingImageContract) return;

    transitionImage(commandBuffer, source.image, source.layout,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    if (descriptor_.clearReadbackBuffer) {
        vkCmdFillBuffer(commandBuffer, readbackBuffer_, 0,
                        static_cast<VkDeviceSize>(descriptor_.readbackSize), 0);
        VkBufferMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_SHADER_WRITE_BIT;
        clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.buffer = readbackBuffer_;
        clearBarrier.size = descriptor_.readbackSize;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &clearBarrier, 0, nullptr);
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = source.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = readbackBuffer_;
    bufferInfo.range = descriptor_.readbackSize;
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(static_cast<size_t>(inputCount()) + 1);
    for (int32_t index = 0; index < inputCount(); ++index) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                          descriptorSet_, static_cast<uint32_t>(index), 0, 1,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfo, nullptr,
                          nullptr});
    }
    writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                      descriptorSet_, descriptor_.readbackBinding, 0, 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufferInfo,
                      nullptr});
    vkUpdateDescriptorSets(context_.device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdDispatch(commandBuffer, (source.extent.width + 15) / 16,
                  (source.extent.height + 15) / 16, 1);

    VkBufferMemoryBarrier readBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.buffer = readbackBuffer_;
    readBarrier.size = descriptor_.readbackSize;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &readBarrier, 0, nullptr);
    transitionImage(commandBuffer, source.image, VK_IMAGE_LAYOUT_GENERAL,
                    source.layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT);
    for (int32_t index = 0; index < outputCount(); ++index) {
        if (index == 0) setOutput(index, source);
    }
}

void VulkanReadbackNode::setCompletion(VulkanSyncPoint completion) {
    VulkanNode::setCompletion(completion);
    readbackReady_ = completion;
}

bool VulkanReadbackNode::readback(void* destination, size_t size) const {
    if (!destination || !readbackMapped_ || size < descriptor_.readbackSize) {
        return false;
    }
    if (readbackReady_.valid()) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &readbackReady_.semaphore;
        waitInfo.pValues = &readbackReady_.value;
        if (vkWaitSemaphores(context_.device, &waitInfo, UINT64_MAX)
            != VK_SUCCESS) return false;
    }
    std::memcpy(destination, readbackMapped_, descriptor_.readbackSize);
    return true;
}

void VulkanReadbackNode::destroyResources() {
    if (!context_.device) return;
    if (readbackMapped_ && readbackMemory_)
        vkUnmapMemory(context_.device, readbackMemory_);
    if (pipeline_) vkDestroyPipeline(context_.device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(context_.device, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(context_.device, descriptorPool_, nullptr);
    if (descriptorSetLayout_)
        vkDestroyDescriptorSetLayout(context_.device, descriptorSetLayout_, nullptr);
    if (readbackBuffer_) vkDestroyBuffer(context_.device, readbackBuffer_, nullptr);
    if (readbackMemory_) vkFreeMemory(context_.device, readbackMemory_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    readbackBuffer_ = VK_NULL_HANDLE;
    readbackMemory_ = VK_NULL_HANDLE;
    readbackMapped_ = nullptr;
    readbackReady_ = {};
}

} // namespace heisenberg::filtergraph
