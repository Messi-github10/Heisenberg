#include "VulkanComputeNode.hpp"
#include "../ShaderBindingContract.hpp"
#include <Utiles/Logger.hpp>
#include <volk.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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

VkDeviceSize alignedUniformSize(size_t size) {
    constexpr VkDeviceSize alignment = 16;
    return std::max<VkDeviceSize>(
        alignment, (static_cast<VkDeviceSize>(size) + alignment - 1)
            & ~(alignment - 1));
}

bool isSampled(VulkanInputBinding binding) {
    return binding != VulkanInputBinding::storageImage;
}

} // namespace

VulkanComputeNode::VulkanComputeNode(
    std::string mark, int32_t inputCount, int32_t outputCount)
    : VulkanNode(std::move(mark), inputCount, outputCount) {
    if (inputCount < 1 || outputCount < 1) {
        throw std::invalid_argument(
            "FilterGraph compute nodes require at least one input and output");
    }
}

VulkanComputeNode::~VulkanComputeNode() {
    destroyPipeline();
}

bool VulkanComputeNode::supportsFormat(ImageType format) const {
    return format == kWorkingImageContract.imageType;
}

bool VulkanComputeNode::configure(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != static_cast<size_t>(inputCount())) return false;
    for (int32_t index = 0; index < inputCount(); ++index) {
        const ImageFormat& format = inputs[static_cast<size_t>(index)];
        if (format.width <= 0 || format.height <= 0
            || !supportsFormat(format.imageType)) {
            return false;
        }
        setInputFormat(index, format);
    }
    return configureOutputs(inputs);
}

bool VulkanComputeNode::configureOutputs(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.empty()) return false;
    for (int32_t index = 0; index < outputCount(); ++index) {
        setOutputFormat(index, inputs[0]);
    }
    return true;
}

VulkanInputBinding VulkanComputeNode::inputBinding(int32_t) const {
    return VulkanInputBinding::storageImage;
}

int32_t VulkanComputeNode::extraInputCount() const {
    return 0;
}

VulkanInputBinding VulkanComputeNode::extraInputBinding(int32_t) const {
    return VulkanInputBinding::sampledLinear;
}

VulkanImageRef VulkanComputeNode::extraInput(int32_t) const {
    return {};
}

void VulkanComputeNode::setExtraInputLayout(int32_t, VkImageLayout) {}

VkExtent3D VulkanComputeNode::workGroupSize() const {
    return {16, 16, 1};
}

void VulkanComputeNode::setUniformBufferSize(size_t size) {
    if (context_.device) {
        throw std::logic_error(
            "FilterGraph UBO size must be set before graph preparation");
    }
    std::lock_guard<std::mutex> lock(uniformMutex_);
    uniformData_.assign(size, 0);
    uniformDirty_ = size > 0;
}

void VulkanComputeNode::updateUniformData(const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(uniformMutex_);
    if (size != uniformData_.size() || (size > 0 && !data)) {
        throw std::invalid_argument("FilterGraph UBO update size mismatch");
    }
    if (size > 0) std::memcpy(uniformData_.data(), data, size);
    uniformDirty_ = size > 0;
}

bool VulkanComputeNode::prepare(const VulkanGraphContext& context) {
    if (!context.device || !context.physicalDevice) return false;
    if (context_.device && context_.device != context.device) {
        destroyPipeline();
        outputImages_.clear();
    }
    context_ = context;

    if (outputImages_.empty()) {
        outputImages_.reserve(static_cast<size_t>(outputCount()));
        for (int32_t index = 0; index < outputCount(); ++index) {
            outputImages_.push_back(
                std::make_unique<VulkanImageResource>(context_));
        }
    }

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int32_t index = 0; index < outputCount(); ++index) {
        const ImageFormat& format =
            outputFormats()[static_cast<size_t>(index)];
        if (!supportsFormat(format.imageType)
            || !outputImages_[static_cast<size_t>(index)]->ensure(
                {static_cast<uint32_t>(format.width),
                 static_cast<uint32_t>(format.height)},
                kWorkingImageContract.format, usage,
                kWorkingImageContract)) {
            return false;
        }
    }

    return pipeline_ || initializePipeline();
}

uint32_t VulkanComputeNode::findMemoryType(
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

bool VulkanComputeNode::initializeUniformBuffer() {
    if (uniformData_.empty()) return true;

    uniformAllocationSize_ = alignedUniformSize(uniformData_.size());
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = uniformAllocationSize_;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context_.device, &bufferInfo, nullptr, &uniformBuffer_)
        != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context_.device, uniformBuffer_, &requirements);
    const uint32_t memoryType = findMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(context_.device, &allocation, nullptr, &uniformMemory_)
            != VK_SUCCESS
        || vkBindBufferMemory(context_.device, uniformBuffer_, uniformMemory_, 0)
            != VK_SUCCESS
        || vkMapMemory(context_.device, uniformMemory_, 0,
                       uniformAllocationSize_, 0, &uniformMapped_)
            != VK_SUCCESS) {
        return false;
    }
    return uploadUniformData();
}

bool VulkanComputeNode::initializeSamplers() {
    bool needsLinear = false;
    bool needsNearest = false;
    for (int32_t index = 0; index < inputCount(); ++index) {
        const VulkanInputBinding binding = inputBinding(index);
        needsLinear |= binding == VulkanInputBinding::sampledLinear;
        needsNearest |= binding == VulkanInputBinding::sampledNearest;
    }
    for (int32_t index = 0; index < extraInputCount(); ++index) {
        const VulkanInputBinding binding = extraInputBinding(index);
        needsLinear |= binding == VulkanInputBinding::sampledLinear;
        needsNearest |= binding == VulkanInputBinding::sampledNearest;
    }

    auto createSampler = [&](VkFilter filter, VkSampler* sampler) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        return vkCreateSampler(context_.device, &info, nullptr, sampler)
            == VK_SUCCESS;
    };

    if (needsLinear && !createSampler(VK_FILTER_LINEAR, &linearSampler_)) {
        return false;
    }
    if (needsNearest && !createSampler(VK_FILTER_NEAREST, &nearestSampler_)) {
        return false;
    }
    return true;
}

bool VulkanComputeNode::initializePipeline() {
    if (!initializeUniformBuffer() || !initializeSamplers()) {
        destroyPipeline();
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(static_cast<size_t>(
        inputCount() + extraInputCount() + outputCount() + 1));
    uint32_t storageCount = static_cast<uint32_t>(outputCount());
    uint32_t sampledCount = 0;
    const uint32_t inputCountValue = static_cast<uint32_t>(inputCount());
    const uint32_t extraInputCountValue =
        static_cast<uint32_t>(extraInputCount());
    const uint32_t outputCountValue = static_cast<uint32_t>(outputCount());

    for (int32_t index = 0; index < inputCount(); ++index) {
        const VkDescriptorType type = isSampled(inputBinding(index))
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings.push_back({shader_abi::inputBinding(
                                static_cast<uint32_t>(index)),
                            type, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ++storageCount;
        else ++sampledCount;
    }
    for (int32_t index = 0; index < extraInputCount(); ++index) {
        const VkDescriptorType type = isSampled(extraInputBinding(index))
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings.push_back({shader_abi::extraInputBinding(
                                inputCountValue,
                                static_cast<uint32_t>(index)),
                            type, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ++storageCount;
        else ++sampledCount;
    }
    for (int32_t index = 0; index < outputCount(); ++index) {
        bindings.push_back({shader_abi::outputBinding(
                                inputCountValue, extraInputCountValue,
                                static_cast<uint32_t>(index)),
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    if (!uniformData_.empty()) {
        bindings.push_back({shader_abi::uniformBinding(
                                inputCountValue, extraInputCountValue,
                                outputCountValue),
                            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }

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

    std::vector<VkDescriptorPoolSize> poolSizes;
    poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageCount});
    if (sampledCount > 0) {
        poolSizes.push_back(
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampledCount});
    }
    if (!uniformData_.empty()) {
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
    }
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
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

    const std::vector<uint32_t> code = loadShaderCode(shaderPath());
    if (code.empty()) {
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

void VulkanComputeNode::destroyUniformBuffer() {
    if (!context_.device) return;
    if (uniformMapped_ && uniformMemory_) {
        vkUnmapMemory(context_.device, uniformMemory_);
    }
    if (uniformBuffer_) {
        vkDestroyBuffer(context_.device, uniformBuffer_, nullptr);
    }
    if (uniformMemory_) {
        vkFreeMemory(context_.device, uniformMemory_, nullptr);
    }
    uniformMapped_ = nullptr;
    uniformBuffer_ = VK_NULL_HANDLE;
    uniformMemory_ = VK_NULL_HANDLE;
    uniformAllocationSize_ = 0;
}

void VulkanComputeNode::destroyPipeline() {
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
    if (linearSampler_) {
        vkDestroySampler(context_.device, linearSampler_, nullptr);
    }
    if (nearestSampler_) {
        vkDestroySampler(context_.device, nearestSampler_, nullptr);
    }
    destroyUniformBuffer();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    linearSampler_ = VK_NULL_HANDLE;
    nearestSampler_ = VK_NULL_HANDLE;
}

VkSampler VulkanComputeNode::samplerFor(
    VulkanInputBinding binding) const {
    return binding == VulkanInputBinding::sampledNearest
        ? nearestSampler_ : linearSampler_;
}

bool VulkanComputeNode::updateDescriptors() {
    const size_t imageCount = static_cast<size_t>(
        inputCount() + extraInputCount() + outputCount());
    const size_t writeCount = imageCount + (uniformData_.empty() ? 0u : 1u);
    std::vector<VkDescriptorImageInfo> imageInfos(imageCount);
    std::vector<VkWriteDescriptorSet> writes(writeCount);
    size_t imageIndex = 0;
    size_t writeIndex = 0;
    const uint32_t inputCountValue = static_cast<uint32_t>(inputCount());
    const uint32_t extraInputCountValue =
        static_cast<uint32_t>(extraInputCount());
    const uint32_t outputCountValue = static_cast<uint32_t>(outputCount());

    for (int32_t index = 0; index < inputCount(); ++index) {
        const VulkanImageRef& source = input(index);
        const VulkanInputBinding binding = inputBinding(index);
        const bool sampled = isSampled(binding);
        const VkImageUsageFlagBits requiredUsage = sampled
            ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
        if (!source.view || !(source.usage & requiredUsage)) {
            LOG_ERROR("FilterGraph: input {} is missing required Vulkan usage",
                      index);
            return false;
        }
        VkDescriptorImageInfo& imageInfo = imageInfos[imageIndex++];
        imageInfo.sampler = sampled ? samplerFor(binding) : VK_NULL_HANDLE;
        imageInfo.imageView = source.view;
        imageInfo.imageLayout = sampled
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet& write = writes[writeIndex++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = shader_abi::inputBinding(
            static_cast<uint32_t>(index));
        write.descriptorCount = 1;
        write.descriptorType = sampled
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
    }

    for (int32_t index = 0; index < extraInputCount(); ++index) {
        const VulkanImageRef source = extraInput(index);
        const VulkanInputBinding binding = extraInputBinding(index);
        const bool sampled = isSampled(binding);
        const VkImageUsageFlagBits requiredUsage = sampled
            ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
        if (!source.valid() || !source.view || !(source.usage & requiredUsage)) {
            LOG_ERROR("FilterGraph: extra input {} is missing required Vulkan usage",
                      index);
            return false;
        }
        VkDescriptorImageInfo& imageInfo = imageInfos[imageIndex++];
        imageInfo.sampler = sampled ? samplerFor(binding) : VK_NULL_HANDLE;
        imageInfo.imageView = source.view;
        imageInfo.imageLayout = sampled
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet& write = writes[writeIndex++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = shader_abi::extraInputBinding(
            inputCountValue, static_cast<uint32_t>(index));
        write.descriptorCount = 1;
        write.descriptorType = sampled
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
    }

    for (int32_t index = 0; index < outputCount(); ++index) {
        const VulkanImageRef destination =
            outputImages_[static_cast<size_t>(index)]->ref();
        VkDescriptorImageInfo& imageInfo = imageInfos[imageIndex++];
        imageInfo.imageView = destination.view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet& write = writes[writeIndex++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = shader_abi::outputBinding(
            inputCountValue, extraInputCountValue,
            static_cast<uint32_t>(index));
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
    }

    VkDescriptorBufferInfo uniformInfo{};
    if (!uniformData_.empty()) {
        uniformInfo.buffer = uniformBuffer_;
        uniformInfo.range = uniformAllocationSize_;
        VkWriteDescriptorSet& write = writes[writeIndex];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = shader_abi::uniformBinding(
            inputCountValue, extraInputCountValue, outputCountValue);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &uniformInfo;
    }

    vkUpdateDescriptorSets(context_.device,
                           static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
    return true;
}

bool VulkanComputeNode::uploadUniformData() {
    std::lock_guard<std::mutex> lock(uniformMutex_);
    if (uniformData_.empty()) return true;
    if (!uniformMapped_) return false;
    if (uniformDirty_) {
        std::memcpy(uniformMapped_, uniformData_.data(), uniformData_.size());
        uniformDirty_ = false;
    }
    return true;
}

void VulkanComputeNode::record(
    VkCommandBuffer commandBuffer, const FrameContext&) {
    if (!pipeline_ || outputImages_.empty() || !uploadUniformData()) return;

    std::vector<VkImageLayout> originalLayouts(
        static_cast<size_t>(inputCount()));
    for (int32_t index = 0; index < inputCount(); ++index) {
        const VulkanImageRef& source = input(index);
        if (!source.valid() || !source.view
            || source.contract != kWorkingImageContract) {
            LOG_ERROR("FilterGraph: compute input {} violates the working contract",
                      index);
            return;
        }
        originalLayouts[static_cast<size_t>(index)] = source.layout;
        const VkImageLayout targetLayout = isSampled(inputBinding(index))
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        transitionImage(commandBuffer, source.image, source.layout, targetLayout,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
    }

    std::vector<VkImageLayout> extraOriginalLayouts(
        static_cast<size_t>(extraInputCount()));
    for (int32_t index = 0; index < extraInputCount(); ++index) {
        const VulkanImageRef source = extraInput(index);
        if (!source.valid() || !source.view
            || source.contract != kWorkingImageContract) {
            LOG_ERROR("FilterGraph: extra compute input {} violates the working contract",
                      index);
            return;
        }
        extraOriginalLayouts[static_cast<size_t>(index)] = source.layout;
        const VkImageLayout targetLayout = isSampled(extraInputBinding(index))
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        transitionImage(commandBuffer, source.image, source.layout, targetLayout,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
        setExtraInputLayout(index, targetLayout);
    }

    for (int32_t index = 0; index < outputCount(); ++index) {
        VulkanImageResource& output =
            *outputImages_[static_cast<size_t>(index)];
        const VulkanImageRef destination = output.ref();
        transitionImage(commandBuffer, destination.image, output.layout(),
                        VK_IMAGE_LAYOUT_GENERAL,
                        output.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        output.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                            ? 0
                            : VK_ACCESS_MEMORY_READ_BIT
                                | VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT);
    }

    if (!updateDescriptors()) return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

    const VulkanImageRef dispatchImage = outputImages_[0]->ref();
    const VkExtent3D group = workGroupSize();
    if (group.width == 0 || group.height == 0 || group.depth == 0) {
        LOG_ERROR("FilterGraph: compute work group size cannot be zero");
        return;
    }
    vkCmdDispatch(commandBuffer,
                  (dispatchImage.extent.width + group.width - 1) / group.width,
                  (dispatchImage.extent.height + group.height - 1) / group.height,
                  1);

    for (int32_t index = 0; index < inputCount(); ++index) {
        const VulkanImageRef& source = input(index);
        const VkImageLayout usedLayout = isSampled(inputBinding(index))
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        transitionImage(commandBuffer, source.image, usedLayout,
                        originalLayouts[static_cast<size_t>(index)],
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    }

    for (int32_t index = 0; index < extraInputCount(); ++index) {
        const VulkanImageRef source = extraInput(index);
        const VkImageLayout usedLayout = isSampled(extraInputBinding(index))
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        const VkImageLayout originalLayout =
            extraOriginalLayouts[static_cast<size_t>(index)];
        transitionImage(commandBuffer, source.image, usedLayout, originalLayout,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
        setExtraInputLayout(index, originalLayout);
    }

    for (int32_t index = 0; index < outputCount(); ++index) {
        VulkanImageResource& output =
            *outputImages_[static_cast<size_t>(index)];
        const VulkanImageRef destination = output.ref();
        transitionImage(commandBuffer, destination.image,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        output.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        setOutput(index, output.ref());
    }
}

} // namespace heisenberg::filtergraph
