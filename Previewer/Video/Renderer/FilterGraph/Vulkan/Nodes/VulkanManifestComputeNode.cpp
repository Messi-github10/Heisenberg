#include "VulkanManifestComputeNode.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Graph/ResourceManager.hpp>
#include <Utiles/Logger.hpp>

#include <volk.h>
#include <bit>
#include <cstdint>
#include <limits>
#include <cstring>
#include <variant>

namespace {

void transitionAuxiliaryImage(VkCommandBuffer commandBuffer, VkImage image,
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

uint16_t floatToHalf(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 0x3c00;
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = ((bits >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = (bits >> 13) & 0x3ff;
    return static_cast<uint16_t>((exponent << 10) | mantissa);
}

} // namespace

#ifndef HEISENBERG_FILTER_SHADER_BINARY_DIR
#define HEISENBERG_FILTER_SHADER_BINARY_DIR "."
#endif

namespace heisenberg::filtergraph {

VulkanManifestComputeNode::VulkanManifestComputeNode(
    const VulkanFilterDescriptor& descriptor,
    const VulkanGraphParameter& parameter)
    : VulkanComputeNode(descriptor.displayName, descriptor.inputCount,
                        descriptor.outputCount),
      descriptor_(descriptor),
      shaderPath_(std::string(HEISENBERG_FILTER_SHADER_BINARY_DIR) + "/"
                  + descriptor.shaderBinary),
      parameters_(parameter) {
    externalInputs_.resize(descriptor_.extraInputs.size());
    setUniformBufferSize(descriptor_.uniformSize);
    updateUniform(parameters_);
}

VulkanManifestComputeNode::~VulkanManifestComputeNode() {
    destroyAuxiliaryUpload();
}

bool VulkanManifestComputeNode::setExternalInput(
    int32_t index, const VulkanImageRef& image) {
    if (index < 0 || static_cast<size_t>(index) >= externalInputs_.size()) {
        return false;
    }
    if (!image.valid() || !image.view
        || image.contract != kWorkingImageContract
        || (descriptor_.auxiliaryWidth != 0
            && image.extent.width != descriptor_.auxiliaryWidth)
        || (descriptor_.auxiliaryHeight != 0
            && image.extent.height != descriptor_.auxiliaryHeight)
        || (extraInputBinding(index) == VulkanInputBinding::sampledLinear
            && !(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        || (extraInputBinding(index) == VulkanInputBinding::sampledNearest
            && !(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        || (extraInputBinding(index) == VulkanInputBinding::storageImage
            && !(image.usage & VK_IMAGE_USAGE_STORAGE_BIT))) {
        return false;
    }
    const size_t slot = static_cast<size_t>(index);
    const bool wasExternal = externalInputs_[slot].valid();
    externalInputs_[slot] = image;

    if (slot < externalInputIds_.size()
        && externalInputIds_[slot].valid() && resourceManager()) {
        if (!resourceManager()->updateImported(externalInputIds_[slot], image)) {
            return false;
        }
    } else if (!wasExternal) {
        invalidateGraph();
    }

    return true;
}

void VulkanManifestComputeNode::updateUniform(
    const VulkanGraphParameter& parameters) {
    if (descriptor_.uniformSize == 0) return;
    std::vector<uint8_t> data(descriptor_.uniformSize, 0);
    for (const VulkanFilterParameterDesc& field : descriptor_.parameters) {
        if (field.offset >= data.size()) continue;
        const auto found = parameters.find(field.name);
        const size_t available = data.size() - field.offset;
        switch (field.type) {
            case VulkanFilterValueType::integer: {
                int32_t number = static_cast<int32_t>(field.defaultValue);
                if (found != parameters.end()) {
                    if (const auto* value = std::get_if<int32_t>(&found->second)) {
                        number = *value;
                    }
                }
                if (available >= sizeof(number)) {
                    std::memcpy(data.data() + field.offset, &number, sizeof(number));
                }
                break;
            }
            case VulkanFilterValueType::real: {
                float number = static_cast<float>(field.defaultValue);
                if (found != parameters.end()) {
                    if (const auto* value = std::get_if<float>(&found->second)) {
                        number = *value;
                    }
                }
                if (available >= sizeof(number)) {
                    std::memcpy(data.data() + field.offset, &number, sizeof(number));
                }
                break;
            }
            case VulkanFilterValueType::boolean: {
                int32_t number = field.defaultValue != 0.0 ? 1 : 0;
                if (found != parameters.end()) {
                    if (const auto* value = std::get_if<bool>(&found->second)) {
                        number = *value ? 1 : 0;
                    }
                }
                if (available >= sizeof(number)) {
                    std::memcpy(data.data() + field.offset, &number, sizeof(number));
                }
                break;
            }
        }
    }
    updateUniformData(data.data(), data.size());
}

bool VulkanManifestComputeNode::configureOutputs(
    const std::vector<ImageFormat>& inputs) {
    if (inputs.empty()) return false;
    for (int32_t index = 0; index < outputCount(); ++index) {
        ImageFormat output = inputs[0];
        if (descriptor_.resizeOutput) {
            if (const auto found = parameters_.find("width");
                found != parameters_.end()) {
                if (const auto* value = std::get_if<int32_t>(&found->second)) {
                    output.width = *value;
                }
            }
            if (const auto found = parameters_.find("height");
                found != parameters_.end()) {
                if (const auto* value = std::get_if<int32_t>(&found->second)) {
                    output.height = *value;
                }
            }
            if (output.width <= 0 || output.height <= 0) return false;
        }
        setOutputFormat(index, output);
    }
    return true;
}

VulkanInputBinding VulkanManifestComputeNode::inputBinding(int32_t inputIndex) const {
    if (inputIndex >= 0
        && static_cast<size_t>(inputIndex) < descriptor_.inputBindings.size()) {
        return descriptor_.inputBindings[static_cast<size_t>(inputIndex)];
    }
    return VulkanInputBinding::storageImage;
}

int32_t VulkanManifestComputeNode::extraInputCount() const {
    return static_cast<int32_t>(descriptor_.extraInputs.size());
}

VulkanInputBinding VulkanManifestComputeNode::extraInputBinding(
    int32_t inputIndex) const {
    if (inputIndex >= 0
        && static_cast<size_t>(inputIndex) < descriptor_.extraInputs.size()) {
        return descriptor_.extraInputs[static_cast<size_t>(inputIndex)].binding;
    }
    return VulkanInputBinding::sampledLinear;
}

VulkanImageRef VulkanManifestComputeNode::extraInput(int32_t inputIndex) const {
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= externalInputs_.size()) {
        return {};
    }
    if (static_cast<size_t>(inputIndex) < externalInputIds_.size()
        && externalInputIds_[static_cast<size_t>(inputIndex)].valid()
        && resourceManager()) {
        VulkanImageRef external = resourceManager()->getResource(
            externalInputIds_[static_cast<size_t>(inputIndex)]);
        if (external.valid()) return external;
    }

    if (auxiliaryResource_.valid() && resourceManager()) {
        return resourceManager()->getResource(auxiliaryResource_);
    }
    return {};
}

void VulkanManifestComputeNode::setExtraInputLayout(
    int32_t inputIndex, VkImageLayout layout) {
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= externalInputs_.size()) {
        return;
    }
    const VulkanImageRef& external = externalInputs_[static_cast<size_t>(inputIndex)];
    if (!external.valid() && auxiliaryResource_.valid() && resourceManager()) {
        LogicalResourceState state = resourceManager()->getState(auxiliaryResource_);
        state.layout = layout;
        resourceManager()->setState(auxiliaryResource_, state);
    }
}

uint32_t VulkanManifestComputeNode::findMemoryType(
    const VulkanGraphContext& context, uint32_t bits,
    VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index))
            && (properties.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

bool VulkanManifestComputeNode::initializeAuxiliaryUpload(
    const VulkanGraphContext& context) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(descriptor_.auxiliaryWidth)
        * descriptor_.auxiliaryHeight * 4 * sizeof(uint16_t);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context.device, &bufferInfo, nullptr,
                       &auxiliaryUploadBuffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, auxiliaryUploadBuffer_, &requirements);
    const uint32_t memoryType = findMemoryType(
        context, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(context.device, &allocation, nullptr,
                         &auxiliaryUploadMemory_) != VK_SUCCESS
        || vkBindBufferMemory(context.device, auxiliaryUploadBuffer_,
                              auxiliaryUploadMemory_, 0) != VK_SUCCESS
        || vkMapMemory(context.device, auxiliaryUploadMemory_, 0, size, 0,
                       &auxiliaryUploadMapped_) != VK_SUCCESS) return false;

    auto* pixels = static_cast<uint16_t*>(auxiliaryUploadMapped_);
    for (uint32_t blue = 0; blue < 64; ++blue) {
        for (uint32_t green = 0; green < 64; ++green) {
            for (uint32_t red = 0; red < 64; ++red) {
                const uint32_t x = (blue % 8) * 64 + red;
                const uint32_t y = (blue / 8) * 64 + green;
                const size_t offset = (static_cast<size_t>(y)
                    * descriptor_.auxiliaryWidth + x) * 4;
                pixels[offset] = floatToHalf(static_cast<float>(red) / 63.0f);
                pixels[offset + 1] = floatToHalf(static_cast<float>(green) / 63.0f);
                pixels[offset + 2] = floatToHalf(static_cast<float>(blue) / 63.0f);
                pixels[offset + 3] = 0x3c00;
            }
        }
    }
    return true;
}

bool VulkanManifestComputeNode::initializeIdentityAuxiliary(
    const VulkanGraphContext& context) {
    if (descriptor_.auxiliaryWidth == 0 || descriptor_.auxiliaryHeight == 0) return false;
    // auxiliary 资源现在由 ResourceManager 管理，这里只需要初始化上传缓冲
    return initializeAuxiliaryUpload(context);
}

void VulkanManifestComputeNode::uploadIdentityAuxiliary(
    VkCommandBuffer commandBuffer) {
    if (!auxiliaryUploadPending_ || !auxiliaryResource_.valid() || !resourceManager()) return;

    VulkanImageRef image = resourceManager()->getResource(auxiliaryResource_);
    if (!image.valid()) return;

    transitionAuxiliaryImage(
        commandBuffer, image.image, image.layout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {descriptor_.auxiliaryWidth,
                          descriptor_.auxiliaryHeight, 1};
    vkCmdCopyBufferToImage(commandBuffer, auxiliaryUploadBuffer_, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionAuxiliaryImage(
        commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    LogicalResourceState state;
    state.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    state.stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    state.access = VK_ACCESS_SHADER_READ_BIT;
    resourceManager()->setState(auxiliaryResource_, state);

    auxiliaryUploadPending_ = false;
}

void VulkanManifestComputeNode::destroyAuxiliaryUpload() {
    if (!context_.device) return;
    if (auxiliaryUploadMapped_ && auxiliaryUploadMemory_) {
        vkUnmapMemory(context_.device, auxiliaryUploadMemory_);
    }
    if (auxiliaryUploadBuffer_) vkDestroyBuffer(context_.device,
                                                 auxiliaryUploadBuffer_, nullptr);
    if (auxiliaryUploadMemory_) vkFreeMemory(context_.device,
                                              auxiliaryUploadMemory_, nullptr);
    auxiliaryUploadMapped_ = nullptr;
    auxiliaryUploadBuffer_ = VK_NULL_HANDLE;
    auxiliaryUploadMemory_ = VK_NULL_HANDLE;
}

bool VulkanManifestComputeNode::prepare(const VulkanGraphContext& context) {
    if (!context.device || !context.physicalDevice) return false;
    if (context_.device && context_.device != context.device) {
        destroyAuxiliaryUpload();
        auxiliaryResource_ = LogicalResourceId{};
        auxiliaryUploadPending_ = true;
        externalInputs_.assign(descriptor_.extraInputs.size(), {});
    }
    context_ = context;

    // 如果需要 identity LUT 且还没有外部输入，则初始化
    if (descriptor_.auxiliarySource == "identity_lut"
        && !auxiliaryResource_.valid()
        && (externalInputs_.empty() || !externalInputs_[0].valid())
        && !initializeIdentityAuxiliary(context)) {
        return false;
    }

    return VulkanComputeNode::prepare(context);
}

std::vector<ResourceAccess> VulkanManifestComputeNode::declareResourceAccess() const {
    // 调用基类方法获取输出资源访问
    auto accesses = VulkanComputeNode::declareResourceAccess();

    // 如果有 auxiliary 资源，声明为读取
    if (auxiliaryResource_.valid() && !auxiliaryUploadPending_) {
        ResourceAccess auxAccess;
        auxAccess.resource = auxiliaryResource_;
        auxAccess.mode = ResourceAccessMode::Read;
        auxAccess.expectedState.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auxAccess.expectedState.stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        auxAccess.expectedState.access = VK_ACCESS_SHADER_READ_BIT;
        accesses.push_back(auxAccess);
    }

    for (size_t i = 0; i < externalInputIds_.size(); ++i) {
        const LogicalResourceId id = externalInputIds_[i];
        if (!id.valid()) continue;
        ResourceAccess access;
        access.resource = id;
        access.mode = ResourceAccessMode::Read;
        const bool sampled = extraInputBinding(static_cast<int32_t>(i))
            != VulkanInputBinding::storageImage;
        access.expectedState.layout = sampled
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        access.expectedState.stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access.expectedState.access = VK_ACCESS_SHADER_READ_BIT;
        accesses.push_back(access);
    }

    return accesses;
}

std::vector<LogicalResourceRequest>
VulkanManifestComputeNode::declareResourceRequests() const {
    auto requests = VulkanComputeNode::declareResourceRequests();
    if (descriptor_.auxiliarySource == "identity_lut"
        && descriptor_.auxiliaryWidth > 0
        && descriptor_.auxiliaryHeight > 0
        && (externalInputs_.empty() || !externalInputs_[0].valid())) {
        LogicalResourceRequest request;
        request.kind = LogicalResourceKind::GraphCreated;
        request.extent = {descriptor_.auxiliaryWidth, descriptor_.auxiliaryHeight};
        request.usage = VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        request.contract = kWorkingImageContract;
        request.name = "auxiliary";
        requests.push_back(std::move(request));
    }

    for (size_t i = 0; i < externalInputs_.size(); ++i) {
        if (!externalInputs_[i].valid()) continue;
        LogicalResourceRequest request;
        request.kind = LogicalResourceKind::External;
        request.name = std::string("external:") + std::to_string(i);
        request.imported = externalInputs_[i];
        request.extent = request.imported.extent;
        request.usage = request.imported.usage;
        request.contract = request.imported.contract;
        requests.push_back(std::move(request));
    }
    return requests;
}

void VulkanManifestComputeNode::bindDeclaredResources(
    const std::vector<LogicalResourceId>& resources) {
    VulkanComputeNode::bindDeclaredResources(resources);
    size_t offset = static_cast<size_t>(outputCount());
    auxiliaryResource_ = {};
    if (descriptor_.auxiliarySource == "identity_lut"
        && descriptor_.auxiliaryWidth > 0
        && descriptor_.auxiliaryHeight > 0
        && (externalInputs_.empty() || !externalInputs_[0].valid())) {
        if (offset < resources.size()) auxiliaryResource_ = resources[offset++];
    }
    externalInputIds_.assign(externalInputs_.size(), {});
    for (size_t i = 0; i < externalInputIds_.size(); ++i) {
        if (!externalInputs_[i].valid()) continue;
        if (offset >= resources.size()) break;
        externalInputIds_[i] = resources[offset++];
    }
}

void VulkanManifestComputeNode::record(
    VkCommandBuffer commandBuffer, const FrameContext& frame) {
    uploadIdentityAuxiliary(commandBuffer);
    VulkanComputeNode::record(commandBuffer, frame);
}

const char* VulkanManifestComputeNode::shaderPath() const {
    return shaderPath_.c_str();
}

} // namespace heisenberg::filtergraph
