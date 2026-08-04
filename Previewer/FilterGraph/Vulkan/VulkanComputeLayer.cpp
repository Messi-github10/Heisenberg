#include "VulkanComputeLayer.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

#include <array>
#include <fstream>
#include <utility>
#include <vector>

#ifndef HEISENBERG_COLOR_INVERT_SHADER_PATH
#error HEISENBERG_COLOR_INVERT_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {
namespace {

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

std::vector<uint32_t> loadShaderCode(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("FilterGraph: failed to open shader '{}'", path);
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0 || size % static_cast<std::streamsize>(sizeof(uint32_t)) != 0) {
        LOG_ERROR("FilterGraph: shader '{}' has an invalid byte size", path);
        return {};
    }

    std::vector<uint32_t> code(
        static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
        LOG_ERROR("FilterGraph: failed to read shader '{}'", path);
        return {};
    }
    return code;
}

} // namespace

VulkanComputeLayer::VulkanComputeLayer(std::string mark)
    : VulkanLayer(std::move(mark), 1, 1) {}

VulkanComputeLayer::~VulkanComputeLayer() {
    destroyPipeline();
}

bool VulkanComputeLayer::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || !supportsFormat(inputs[0].imageType)) {
        return false;
    }
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    return true;
}

bool VulkanComputeLayer::prepare(const VulkanGraphContext& context) {
    if (!context.device) return false;
    if (context_.device && context_.device != context.device) {
        destroyPipeline();
        outputImage_.reset();
    }
    context_ = context;
    if (!outputImage_) {
        outputImage_ = std::make_unique<VulkanImageResource>(context_);
    }
    if (!pipeline_ && !initializePipeline()) return false;

    const ImageFormat& format = outputFormats()[0];
    return outputImage_->ensure(
        {static_cast<uint32_t>(format.width),
         static_cast<uint32_t>(format.height)},
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

bool VulkanComputeLayer::initializePipeline() {
    const std::vector<uint32_t> code = loadShaderCode(shaderPath());
    if (!context_.device || code.empty()) return false;

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setLayoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(context_.device, &setLayoutInfo, nullptr,
                                    &descriptorSetLayout_) != VK_SUCCESS) {
        destroyPipeline();
        return false;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkCreatePipelineLayout(context_.device, &pipelineLayoutInfo, nullptr,
                               &pipelineLayout_) != VK_SUCCESS) {
        destroyPipeline();
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(context_.device, &poolInfo, nullptr,
                               &descriptorPool_) != VK_SUCCESS) {
        destroyPipeline();
        return false;
    }

    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(context_.device, &setInfo, &descriptorSet_)
        != VK_SUCCESS) {
        destroyPipeline();
        return false;
    }

    VkShaderModuleCreateInfo shaderInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = code.size() * sizeof(uint32_t);
    shaderInfo.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context_.device, &shaderInfo, nullptr, &shader)
        != VK_SUCCESS) {
        destroyPipeline();
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
    if (result != VK_SUCCESS) {
        LOG_ERROR("FilterGraph: compute pipeline creation failed ({})",
                  static_cast<int>(result));
        destroyPipeline();
        return false;
    }
    return true;
}

void VulkanComputeLayer::destroyPipeline() {
    if (!context_.device) return;
    if (pipeline_) vkDestroyPipeline(context_.device, pipeline_, nullptr);
    if (pipelineLayout_) {
        vkDestroyPipelineLayout(context_.device, pipelineLayout_, nullptr);
    }
    if (descriptorPool_) {
        vkDestroyDescriptorPool(context_.device, descriptorPool_, nullptr);
    }
    if (descriptorSetLayout_) {
        vkDestroyDescriptorSetLayout(
            context_.device, descriptorSetLayout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
}

void VulkanComputeLayer::updateDescriptors(
    const VulkanImageRef& source, const VulkanImageRef& destination) {
    const std::array<VkDescriptorImageInfo, 2> images{{
        {VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, destination.view, VK_IMAGE_LAYOUT_GENERAL},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(context_.device,
                           static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void VulkanComputeLayer::record(
    VkCommandBuffer commandBuffer, const FrameContext&) {
    const VulkanImageRef& source = input(0);
    if (!source.valid() || !source.view || !outputImage_ || !pipeline_) {
        LOG_ERROR("FilterGraph: compute layer requires valid Vulkan image views");
        return;
    }

    VulkanImageRef destination = outputImage_->ref();
    if (!destination.valid() || !destination.view) return;

    transitionImage(commandBuffer, source.image, source.layout,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    transitionImage(commandBuffer, destination.image, destination.layout,
                    VK_IMAGE_LAYOUT_GENERAL,
                    destination.layout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    destination.layout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? 0
                        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT);

    updateDescriptors(source, destination);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdDispatch(commandBuffer, (source.extent.width + 15u) / 16u,
                  (source.extent.height + 15u) / 16u, 1);

    transitionImage(commandBuffer, source.image, VK_IMAGE_LAYOUT_GENERAL,
                    source.layout,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    transitionImage(commandBuffer, destination.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    outputImage_->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    setOutput(0, outputImage_->ref());
}

VulkanColorInvertLayer::VulkanColorInvertLayer()
    : VulkanComputeLayer("VulkanColorInvert") {}

bool VulkanColorInvertLayer::supportsFormat(ImageType format) const {
    return format == ImageType::rgba16f;
}

const char* VulkanColorInvertLayer::shaderPath() const {
    return HEISENBERG_COLOR_INVERT_SHADER_PATH;
}

} // namespace heisenberg::filtergraph
