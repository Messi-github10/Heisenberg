#include <volk.h>
#include "ColorPipeline.hpp"
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <shaderc/shaderc.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace heisenberg::renderer::color {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(int(result)) + ")");
}
uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No compatible Vulkan memory type for ColorGpuPass");
}
struct Buffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    explicit Buffer(VkDevice d) : device(d) {}
    void update(VkDeviceSize size, const void* data) {
        if (size > capacity) throw std::invalid_argument("Cached UBO size changed without a new program identity");
        if (!size) return;
        void* mapped = nullptr;
        check(vkMapMemory(device, memory, 0, size, 0, &mapped), "vkMapMemory(UBO)");
        std::memcpy(mapped, data, size_t(size));
        vkUnmapMemory(device, memory);
    }
    ~Buffer() { if (buffer) vkDestroyBuffer(device, buffer, nullptr); if (memory) vkFreeMemory(device, memory, nullptr); }
    void create(const filtergraph::VulkanGraphContext& context, VkDeviceSize size, VkBufferUsageFlags usage, const void* data) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = std::max<VkDeviceSize>(size, 16); info.usage = usage; capacity = size;
        check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(context.physicalDevice, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &alloc, nullptr, &memory), "vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
        if (size && data) {
            void* mapped = nullptr;
            check(vkMapMemory(device, memory, 0, size, 0, &mapped), "vkMapMemory");
            std::memcpy(mapped, data, size_t(size));
            vkUnmapMemory(device, memory);
        }
    }
};
struct LutImage {
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> staging;
    explicit LutImage(VkDevice d) : device(d) {}
    ~LutImage() {
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
    void create(const filtergraph::VulkanGraphContext& context, const GpuLut& lut) {
        if (!lut.width || !lut.height || !lut.depth || (lut.channels != 1 && lut.channels != 3)
            || lut.values.size() != size_t(lut.width) * lut.height * lut.depth * lut.channels)
            throw std::invalid_argument("Invalid GPU LUT dimensions/data");
        const VkFormat format = lut.channels == 1 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, format, &properties);
        VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (lut.linearInterpolation) needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed)
            throw std::runtime_error("Vulkan device does not support required float LUT sampling/interpolation");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = lut.is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = format; info.extent = {lut.width, lut.height, lut.depth};
        info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage(LUT)");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(context.physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &alloc, nullptr, &memory), "vkAllocateMemory(LUT)");
        check(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory(LUT)");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image; viewInfo.viewType = lut.is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format; viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView(LUT)");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = lut.linearInterpolation ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler(LUT)");
        staging = std::make_unique<Buffer>(device);
        if (lut.channels == 1) staging->create(context, lut.values.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, lut.values.data());
        else {
            // OCIO supplies x-fastest RGB texels in the exact order referenced by
            // its shader. Expand RGB -> RGBA, never reindex/bake into an atlas.
            std::vector<float> rgba(lut.values.size() / 3 * 4);
            for (size_t i = 0; i < lut.values.size() / 3; ++i) {
                for (size_t c = 0; c < 3; ++c) rgba[i * 4 + c] = lut.values[i * 3 + c];
                rgba[i * 4 + 3] = 1.0f;
            }
            staging->create(context, rgba.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba.data());
        }
    }
};
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
             VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : srcAccess;
    b.dstAccessMask = dstAccess; b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}
struct SubmissionResources {
    VkDevice device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 2> setLayouts{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkSampler inputSampler = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> ubo;
    std::vector<std::unique_ptr<LutImage>> luts;
    bool submitted = false;
    bool lutsUploaded = false;
    VkCommandBuffer command = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> sets{};
    std::vector<uint32_t> spirv;
    explicit SubmissionResources(VkDevice d) : device(d) {}
    ~SubmissionResources() {
        // Also protects local resources on exceptions following submission.
        if (submitted) vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        for (auto set : setLayouts) if (set) vkDestroyDescriptorSetLayout(device, set, nullptr);
        if (inputSampler) vkDestroySampler(device, inputSampler, nullptr);
    }
};
std::vector<uint32_t> compile(const std::string& source) {
    auto compiler = shaderc_compiler_initialize();
    auto options = shaderc_compile_options_initialize();
    if (!compiler || !options) {
        if (options) shaderc_compile_options_release(options);
        if (compiler) shaderc_compiler_release(compiler);
        throw std::runtime_error("Could not initialize shaderc");
    }
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_5);
    auto result = shaderc_compile_into_spv(compiler, source.data(), source.size(), shaderc_compute_shader,
        "ColorGpuPass.comp", "main", options);
    std::vector<uint32_t> binary;
    std::string error;
    if (!result) error = "shaderc returned no result";
    else if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
        error = shaderc_result_get_error_message(result);
    else {
        binary.resize(shaderc_result_get_length(result) / sizeof(uint32_t));
        std::memcpy(binary.data(), shaderc_result_get_bytes(result), binary.size() * sizeof(uint32_t));
    }
    if (result) shaderc_result_release(result);
    shaderc_compile_options_release(options); shaderc_compiler_release(compiler);
    if (!error.empty()) throw std::runtime_error("OCIO Vulkan shader compile failed: " + error);
    return binary;
}
}
struct ColorGpuPass::Impl {
    filtergraph::VulkanGraphContext context;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t value = 0;
    std::mutex mutex;
    std::function<bool()> cancellation;
    struct Entry {
        std::string id;
        VkFormat format;
        std::unique_ptr<SubmissionResources> resources;
    };
    std::list<Entry> cache; // front is most recently used, never shared between instances
    ColorGpuCacheStats stats;
    void checkCancellation() const {
        if (cancellation && cancellation()) throw std::runtime_error("Cancelled");
    }
    explicit Impl(filtergraph::VulkanGraphContext c) : context(c) {
        if (!c.device || !c.physicalDevice || !c.queue || c.queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED)
            throw std::invalid_argument("ColorGpuPass needs a complete VulkanGraphContext");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext = &type;
        check(vkCreateSemaphore(c.device, &info, nullptr, &timeline), "vkCreateSemaphore(ColorGpuPass)");
    }
    ~Impl() { cache.clear(); if (timeline) vkDestroySemaphore(context.device, timeline, nullptr); }
};
ColorGpuPass::ColorGpuPass(filtergraph::VulkanGraphContext c) : impl_(std::make_unique<Impl>(c)) {}
ColorGpuPass::~ColorGpuPass() = default;
void ColorGpuPass::setCancellation(std::function<bool()> cancellation) {
    std::lock_guard lock(impl_->mutex);
    impl_->cancellation = std::move(cancellation);
}
ColorGpuCacheStats ColorGpuPass::cacheStats() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->stats; result.residentPrograms = impl_->cache.size(); return result;
}
filtergraph::VulkanSyncPoint ColorGpuPass::execute(const GpuProgram& program,
    const filtergraph::VulkanImageRef& input, const filtergraph::VulkanImageRef& output) {
    std::lock_guard lock(impl_->mutex);
    const auto& context = impl_->context;
    impl_->checkCancellation();
    if (!input.valid() || !input.view || !output.valid() || !output.view || input.image == output.image
        || input.extent.width != output.extent.width || input.extent.height != output.extent.height
        || !(input.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(output.usage & VK_IMAGE_USAGE_STORAGE_BIT)
        || input.layout == VK_IMAGE_LAYOUT_UNDEFINED)
        throw std::invalid_argument("ColorGpuPass requires distinct, equal-sized sampled input and storage output images with defined input layout");
    if (input.vkFormat != VK_FORMAT_R16G16B16A16_SFLOAT && input.vkFormat != VK_FORMAT_R32G32B32A32_SFLOAT)
        throw std::invalid_argument("ColorGpuPass input must be unpacked float RGBA; sRGB VkFormats would duplicate transfer decoding");
    for (const auto* ref : {&input, &output})
        if (ref->queueFamilyIndex != VK_QUEUE_FAMILY_IGNORED && ref->queueFamilyIndex != context.queueFamilyIndex)
            throw std::invalid_argument("ColorGpuPass image ownership must be transferred to its queue family by caller");
    const char* qualifier = nullptr;
    if (output.vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT) qualifier = "rgba16f";
    else if (output.vkFormat == VK_FORMAT_R32G32B32A32_SFLOAT) qualifier = "rgba32f";
    else throw std::invalid_argument("ColorGpuPass output must be RGBA16F or RGBA32F, never UNORM");
    if (program.functionName.empty() || program.shaderText.empty()) throw std::invalid_argument("Empty GPU program");
    if (program.cacheId.empty()) throw std::invalid_argument("GPU program needs a stable immutable cacheId");
    const auto uniformData = program.uniformUpdate ? program.uniformUpdate() : program.uniformData;
    if (uniformData.size() != program.uniformData.size())
        throw std::invalid_argument("Dynamic OCIO UBO size changed");
    auto found = std::find_if(impl_->cache.begin(), impl_->cache.end(), [&](const auto& entry) {
        return entry.id == program.cacheId && entry.format == output.vkFormat;
    });
    const bool cold = found == impl_->cache.end();
    std::unique_ptr<SubmissionResources> fresh;
    if (cold) { ++impl_->stats.misses; fresh = std::make_unique<SubmissionResources>(context.device); }
    else { ++impl_->stats.hits; impl_->cache.splice(impl_->cache.begin(), impl_->cache, found); }
    auto& resources = cold ? *fresh : *impl_->cache.front().resources;
    if (cold) {
        std::string source = "#version 460\nlayout(local_size_x=8,local_size_y=8) in;\n"
            "layout(set=0,binding=0) uniform sampler2D colorInput;\nlayout(set=0,binding=1,";
        source += qualifier;
        source += ") writeonly uniform image2D colorOutput;\n";
        source += program.shaderText;
        source += "\nvoid main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(colorOutput))))return;"
                  "vec4 c=texelFetch(colorInput,p,0);vec4 r=" + program.functionName + "(c);r.a=c.a;imageStore(colorOutput,p,r);}\n";
        resources.spirv = compile(source);
        ++impl_->stats.shaderCompilations;
        const auto& binary = resources.spirv;
        impl_->checkCancellation();
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = binary.size() * sizeof(uint32_t); shaderInfo.pCode = binary.data();
        check(vkCreateShaderModule(context.device, &shaderInfo, nullptr, &resources.shader), "vkCreateShaderModule");
        std::vector<VkDescriptorSetLayoutBinding> mainBindings{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        std::vector<VkDescriptorSetLayoutBinding> programBindings;
        if (!program.uniformData.empty()) programBindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        for (const auto& lut : program.textures()) {
            impl_->checkCancellation();
            if (!lut.binding || std::any_of(programBindings.begin(), programBindings.end(), [&](const auto& b){return b.binding == lut.binding;}))
                throw std::invalid_argument("Duplicate/reserved GPU LUT binding");
            programBindings.push_back({lut.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            auto image = std::make_unique<LutImage>(context.device); image->create(context, lut);
            resources.luts.push_back(std::move(image));
        }
        const std::array<const std::vector<VkDescriptorSetLayoutBinding>*, 2> bindings{&mainBindings, &programBindings};
        for (size_t i = 0; i < bindings.size(); ++i) {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = uint32_t(bindings[i]->size()); info.pBindings = bindings[i]->data();
            check(vkCreateDescriptorSetLayout(context.device, &info, nullptr, &resources.setLayouts[i]), "vkCreateDescriptorSetLayout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 2; layoutInfo.pSetLayouts = resources.setLayouts.data();
        check(vkCreatePipelineLayout(context.device, &layoutInfo, nullptr, &resources.layout), "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipelineInfo.stage.module = resources.shader; pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = resources.layout;
        check(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &resources.pipeline), "vkCreateComputePipelines");
        std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(1 + resources.luts.size())}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        if (!program.uniformData.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 2; descriptorInfo.poolSizeCount = uint32_t(sizes.size()); descriptorInfo.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(context.device, &descriptorInfo, nullptr, &resources.descriptorPool), "vkCreateDescriptorPool");
        auto& sets = resources.sets;
        VkDescriptorSetAllocateInfo allocateSets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateSets.descriptorPool = resources.descriptorPool; allocateSets.descriptorSetCount = 2; allocateSets.pSetLayouts = resources.setLayouts.data();
        check(vkAllocateDescriptorSets(context.device, &allocateSets, sets.data()), "vkAllocateDescriptorSets");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST; samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(context.device, &samplerInfo, nullptr, &resources.inputSampler), "vkCreateSampler(input)");
        std::vector<VkDescriptorImageInfo> images(2 + resources.luts.size());
        images[0] = {resources.inputSampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL};
        std::vector<VkWriteDescriptorSet> writes;
        auto imageWrite = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, size_t index) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set; write.dstBinding = binding; write.descriptorCount = 1;
            write.descriptorType = type; write.pImageInfo = &images[index]; writes.push_back(write);
        };
        imageWrite(sets[0], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0);
        imageWrite(sets[0], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1);
        for (size_t i = 0; i < resources.luts.size(); ++i) {
            images[i + 2] = {resources.luts[i]->sampler, resources.luts[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            imageWrite(sets[1], program.textures()[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, i + 2);
        }
        VkDescriptorBufferInfo uboInfo{};
        if (!program.uniformData.empty()) {
            resources.ubo = std::make_unique<Buffer>(context.device);
            resources.ubo->create(context, uniformData.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniformData.data());
            uboInfo = {resources.ubo->buffer, 0, program.uniformData.size()};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = sets[1]; write.dstBinding = 0; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1; write.pBufferInfo = &uboInfo; writes.push_back(write);
        }
        vkUpdateDescriptorSets(context.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = context.queueFamilyIndex;
        check(vkCreateCommandPool(context.device, &poolInfo, nullptr, &resources.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocateCmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateCmd.commandPool = resources.pool; allocateCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocateCmd.commandBufferCount = 1;
        auto& cmd = resources.command;
        check(vkAllocateCommandBuffers(context.device, &allocateCmd, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device, &fenceInfo, nullptr, &resources.fence), "vkCreateFence");
    } else {
        if (bool(resources.ubo) != !uniformData.empty())
            throw std::invalid_argument("Cached GPU program uniform layout changed");
        if (resources.ubo) resources.ubo->update(uniformData.size(), uniformData.data());
        std::array<VkDescriptorImageInfo, 2> images{{
            {resources.inputSampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = resources.sets[0]; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(context.device, 2, writes.data(), 0, nullptr);
        check(vkResetCommandPool(context.device, resources.pool, 0), "vkResetCommandPool");
        check(vkResetFences(context.device, 1, &resources.fence), "vkResetFences");
    }
    // Cached entries survive exceptions, so the submission needs a separate
    // scope guard: no cache reuse/eviction/UBO rewrite until GPU completion.
    struct Drain {
        SubmissionResources& resources;
        bool armed = true;
        ~Drain() {
            if (!armed) return;
            if (resources.submitted) {
                const auto result = vkWaitForFences(resources.device, 1, &resources.fence, VK_TRUE, UINT64_MAX);
                if (result == VK_SUCCESS) resources.lutsUploaded = true;
                resources.submitted = false;
            }
        }
    } drain{resources};
    const auto cmd = resources.command;
    const auto& sets = resources.sets;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    if (!resources.lutsUploaded) for (size_t i = 0; i < resources.luts.size(); ++i) {
        const auto& lut = program.textures()[i]; const auto& image = *resources.luts[i];
        barrier(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {lut.width, lut.height, lut.depth};
        vkCmdCopyBufferToImage(cmd, image.staging->buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    barrier(cmd, input.image, input.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cmd, output.image, output.layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.layout, 0, 2, sets.data(), 0, nullptr);
    vkCmdDispatch(cmd, (output.extent.width + 7) / 8, (output.extent.height + 7) / 8, 1);
    barrier(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    // Caller may still use the input in its original layout after this pass.
    barrier(cmd, input.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.layout, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    std::vector<VkSemaphore> waits; std::vector<uint64_t> waitValues;
    for (const auto& ready : {input.ready, output.ready}) {
        if (!ready.valid()) continue;
        auto it = std::find(waits.begin(), waits.end(), ready.semaphore);
        if (it == waits.end()) { waits.push_back(ready.semaphore); waitValues.push_back(ready.value); }
        else waitValues[size_t(it - waits.begin())] = std::max(waitValues[size_t(it - waits.begin())], ready.value);
    }
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const uint64_t signalValue = ++impl_->value;
    VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = uint32_t(waitValues.size()); timelineInfo.pWaitSemaphoreValues = waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 1; timelineInfo.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext = &timelineInfo;
    submit.waitSemaphoreCount = uint32_t(waits.size()); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &impl_->timeline;
    impl_->checkCancellation();
    check(synchronizedQueueSubmit(context.queue, 1, &submit, resources.fence), "vkQueueSubmit(ColorGpuPass)");
    resources.submitted = true;
    bool cancelled = false;
    for (;;) {
        // Observe cancellation promptly, but never free resources or abandon a
        // semaphore-waiting submission. Drain first, even if the token resets.
        // A callback exception also takes the RAII fence-drain path.
        if (impl_->cancellation && impl_->cancellation()) cancelled = true;
        constexpr uint64_t pollNanoseconds = 10'000'000; // 10 ms, no queue lock
        const auto result = vkWaitForFences(context.device, 1, &resources.fence, VK_TRUE, pollNanoseconds);
        if (result == VK_TIMEOUT) continue;
        check(result, "vkWaitForFences(ColorGpuPass)");
        break;
    }
    resources.submitted = false;
    drain.armed = false;
    if (!resources.lutsUploaded) impl_->stats.lutUploads += resources.luts.size();
    resources.lutsUploaded = true;
    for (auto& lut : resources.luts) lut->staging.reset();
    if (cold) {
        // All cached submissions are synchronous and drained; LRU eviction
        // cannot free resources referenced by a command still in flight.
        if (impl_->cache.size() >= kProgramCacheCapacity) {
            impl_->cache.pop_back(); ++impl_->stats.evictions;
        }
        impl_->cache.push_front({program.cacheId, output.vkFormat, std::move(fresh)});
    }
    if (impl_->cancellation && impl_->cancellation()) cancelled = true;
    if (cancelled) throw std::runtime_error("Cancelled");
    return {impl_->timeline, signalValue};
}
} // namespace heisenberg::renderer::color
