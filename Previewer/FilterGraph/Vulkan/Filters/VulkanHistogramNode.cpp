#include "VulkanHistogramNode.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#ifndef HEISENBERG_HISTOGRAM_SHADER_PATH
#error HEISENBERG_HISTOGRAM_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {
namespace {

constexpr VkDeviceSize kHistogramBufferSize = 256 * sizeof(uint32_t);

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
    std::vector<uint32_t> code(
        static_cast<size_t>(size) / sizeof(uint32_t));
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

VulkanHistogramNode::VulkanHistogramNode()
    : VulkanNode("VulkanHistogram", 1, 1) {}

VulkanHistogramNode::~VulkanHistogramNode() {
    destroyResources();
}

bool VulkanHistogramNode::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || inputs[0].imageType != kWorkingImageContract.imageType) {
        return false;
    }
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    return true;
}

uint32_t VulkanHistogramNode::findMemoryType(
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

bool VulkanHistogramNode::initializeBuffer() {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = kHistogramBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context_.device, &bufferInfo, nullptr, &binsBuffer_)
        != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context_.device, binsBuffer_, &requirements);
    const uint32_t memoryType = findMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    return vkAllocateMemory(context_.device, &allocation, nullptr, &binsMemory_)
            == VK_SUCCESS
        && vkBindBufferMemory(context_.device, binsBuffer_, binsMemory_, 0)
            == VK_SUCCESS
        && vkMapMemory(context_.device, binsMemory_, 0, kHistogramBufferSize,
                       0, &binsMapped_)
            == VK_SUCCESS;
}

bool VulkanHistogramNode::initializePipeline() {
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
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
                               &pipelineLayout_) != VK_SUCCESS) {
        return false;
    }

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    }};
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(context_.device, &poolInfo, nullptr,
                               &descriptorPool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(context_.device, &setInfo, &descriptorSet_)
        != VK_SUCCESS) {
        return false;
    }

    const std::vector<uint32_t> code = loadShaderCode(
        HEISENBERG_HISTOGRAM_SHADER_PATH);
    if (code.empty()) return false;
    VkShaderModuleCreateInfo shaderInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = code.size() * sizeof(uint32_t);
    shaderInfo.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context_.device, &shaderInfo, nullptr, &shader)
        != VK_SUCCESS) {
        return false;
    }

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

bool VulkanHistogramNode::prepare(const VulkanGraphContext& context) {
    if (!context.device || !context.physicalDevice) return false;
    if (context_.device && context_.device != context.device) {
        destroyResources();
    }
    context_ = context;
    if (pipeline_) return true;
    if (!initializeBuffer() || !initializePipeline()) {
        destroyResources();
        return false;
    }
    return true;
}

void VulkanHistogramNode::record(VkCommandBuffer commandBuffer,
                                 const FrameContext&) {
    const VulkanImageRef source = input(0);
    if (!pipeline_ || !source.valid() || !source.view
        || source.contract != kWorkingImageContract) {
        return;
    }

    transitionImage(commandBuffer, source.image, source.layout,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    vkCmdFillBuffer(commandBuffer, binsBuffer_, 0, kHistogramBufferSize, 0);
    VkBufferMemoryBarrier clearBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
        | VK_ACCESS_SHADER_WRITE_BIT;
    clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.buffer = binsBuffer_;
    clearBarrier.size = kHistogramBufferSize;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &clearBarrier, 0, nullptr);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = source.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = binsBuffer_;
    bufferInfo.range = kHistogramBufferSize;
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(context_.device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdDispatch(commandBuffer, (source.extent.width + 15) / 16,
                  (source.extent.height + 15) / 16, 1);

    VkBufferMemoryBarrier readBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.buffer = binsBuffer_;
    readBarrier.size = kHistogramBufferSize;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &readBarrier, 0, nullptr);

    transitionImage(commandBuffer, source.image, VK_IMAGE_LAYOUT_GENERAL,
                    source.layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    setOutput(0, source);
}

void VulkanHistogramNode::setCompletion(VulkanSyncPoint completion) {
    VulkanNode::setCompletion(completion);
    binsReady_ = completion;
}

bool VulkanHistogramNode::readBins(std::array<uint32_t, 256>& bins) const {
    if (!binsMapped_) return false;
    if (binsReady_.valid()) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &binsReady_.semaphore;
        waitInfo.pValues = &binsReady_.value;
        if (vkWaitSemaphores(context_.device, &waitInfo, UINT64_MAX)
            != VK_SUCCESS) {
            return false;
        }
    }
    std::memcpy(bins.data(), binsMapped_, kHistogramBufferSize);
    return true;
}

void VulkanHistogramNode::destroyResources() {
    if (!context_.device) return;
    if (binsMapped_ && binsMemory_) vkUnmapMemory(context_.device, binsMemory_);
    if (pipeline_) vkDestroyPipeline(context_.device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(context_.device, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(context_.device, descriptorPool_, nullptr);
    if (descriptorSetLayout_) {
        vkDestroyDescriptorSetLayout(context_.device, descriptorSetLayout_, nullptr);
    }
    if (binsBuffer_) vkDestroyBuffer(context_.device, binsBuffer_, nullptr);
    if (binsMemory_) vkFreeMemory(context_.device, binsMemory_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    binsBuffer_ = VK_NULL_HANDLE;
    binsMemory_ = VK_NULL_HANDLE;
    binsMapped_ = nullptr;
    binsReady_ = {};
}

} // namespace heisenberg::filtergraph
