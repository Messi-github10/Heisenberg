#include "VulkanManifestComputeNode.hpp"

#include <QJsonValue>
#include <volk.h>
#include <bit>
#include <cstdint>
#include <limits>
#include <cstring>

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
    const VulkanGraphParameter& parameter,
    QJsonObject uniformOverrides)
    : VulkanComputeNode(descriptor.displayName, descriptor.inputCount,
                        descriptor.outputCount),
      descriptor_(descriptor),
      shaderPath_(std::string(HEISENBERG_FILTER_SHADER_BINARY_DIR) + "/"
                  + descriptor.shaderBinary) {
    externalInputs_.resize(descriptor_.extraInputs.size());
    parameters_ = parameterObject(parameter);
    for (auto it = uniformOverrides.begin(); it != uniformOverrides.end(); ++it) {
        parameters_.insert(it.key(), it.value());
    }
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
    externalInputs_[static_cast<size_t>(index)] = image;
    return true;
}

QJsonObject VulkanManifestComputeNode::parameterObject(
    const VulkanGraphParameter& parameter) {
    if (const auto* json = std::get_if<VulkanJsonParameter>(&parameter)) {
        return json->object;
    }
    return {};
}

void VulkanManifestComputeNode::updateUniform(const QJsonObject& object) {
    if (descriptor_.uniformSize == 0) return;
    std::vector<uint8_t> data(descriptor_.uniformSize, 0);
    for (const VulkanFilterParameterDesc& field : descriptor_.parameters) {
        if (field.offset >= data.size()) continue;
        const QJsonValue value = object.contains(QString::fromStdString(field.name))
            ? object.value(QString::fromStdString(field.name))
            : QJsonValue(field.defaultValue);
        const size_t available = data.size() - field.offset;
        switch (field.type) {
            case VulkanFilterValueType::integer: {
                const int32_t number = static_cast<int32_t>(value.toInteger(
                    static_cast<qint64>(field.defaultValue)));
                if (available >= sizeof(number)) {
                    std::memcpy(data.data() + field.offset, &number, sizeof(number));
                }
                break;
            }
            case VulkanFilterValueType::real: {
                const float number = static_cast<float>(value.toDouble(field.defaultValue));
                if (available >= sizeof(number)) {
                    std::memcpy(data.data() + field.offset, &number, sizeof(number));
                }
                break;
            }
            case VulkanFilterValueType::boolean: {
                const int32_t number = value.toBool(field.defaultValue != 0.0) ? 1 : 0;
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
            output.width = parameters_.value("width").toInt(output.width);
            output.height = parameters_.value("height").toInt(output.height);
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
    const VulkanImageRef external = externalInputs_[static_cast<size_t>(inputIndex)];
    if (external.valid()) return external;
    return auxiliaryImage_ ? auxiliaryImage_->ref() : VulkanImageRef{};
}

void VulkanManifestComputeNode::setExtraInputLayout(
    int32_t inputIndex, VkImageLayout layout) {
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= externalInputs_.size()) {
        return;
    }
    VulkanImageRef& external = externalInputs_[static_cast<size_t>(inputIndex)];
    if (external.valid()) {
        external.layout = layout;
    } else if (auxiliaryImage_) {
        auxiliaryImage_->setLayout(layout);
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
    auxiliaryImage_ = std::make_unique<VulkanImageResource>(context);
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return auxiliaryImage_->ensure(
        {descriptor_.auxiliaryWidth, descriptor_.auxiliaryHeight}, usage,
        kWorkingImageContract) && initializeAuxiliaryUpload(context);
}

void VulkanManifestComputeNode::uploadIdentityAuxiliary(
    VkCommandBuffer commandBuffer) {
    if (!auxiliaryUploadPending_ || !auxiliaryImage_) return;
    const VulkanImageRef image = auxiliaryImage_->ref();
    transitionAuxiliaryImage(commandBuffer, image.image, auxiliaryImage_->layout(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {descriptor_.auxiliaryWidth,
                          descriptor_.auxiliaryHeight, 1};
    vkCmdCopyBufferToImage(commandBuffer, auxiliaryUploadBuffer_, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionAuxiliaryImage(commandBuffer, image.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
    auxiliaryImage_->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        auxiliaryImage_.reset();
        auxiliaryUploadPending_ = true;
        externalInputs_.assign(descriptor_.extraInputs.size(), {});
    }
    context_ = context;
    if (descriptor_.auxiliarySource == "identity_lut"
        && !auxiliaryImage_
        && (externalInputs_.empty() || !externalInputs_[0].valid())
        && !initializeIdentityAuxiliary(context)) return false;
    return VulkanComputeNode::prepare(context);
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
