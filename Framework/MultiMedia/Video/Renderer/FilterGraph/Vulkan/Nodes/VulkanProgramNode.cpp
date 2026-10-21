#include <volk.h>
#include "VulkanProgramNode.hpp"
#include <shaderc/shaderc.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {
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
    throw std::runtime_error("No compatible Vulkan memory type for VulkanProgramNode");
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
    void create(const VulkanGraphContext& context, VkDeviceSize size, VkBufferUsageFlags usage, const void* data) {
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
    void create(const VulkanGraphContext& context, const GpuLut& lut) {
        if (!lut.width || !lut.height || !lut.depth || (!lut.is3D && lut.depth != 1) || (lut.channels != 1 && lut.channels != 3)
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
            // Program supplies x-fastest RGB texels in the exact order referenced by
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
        "VulkanProgramNode.comp", "main", options);
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
    if (!error.empty()) throw std::runtime_error("FilterGraph program shader compile failed: " + error);
    return binary;
}
struct ProgramResources {
    VkDevice device;
    std::array<VkDescriptorSetLayout, 2> setLayouts{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkSampler inputSampler = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> sets{};
    std::unique_ptr<Buffer> ubo;
    std::vector<std::unique_ptr<LutImage>> luts;
    bool lutsUploaded = false;
    bool lutUploadRecorded = false;
    explicit ProgramResources(VkDevice d) : device(d) {}
    ~ProgramResources() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        for (auto set : setLayouts) if (set) vkDestroyDescriptorSetLayout(device, set, nullptr);
        if (inputSampler) vkDestroySampler(device, inputSampler, nullptr);
    }
};
} // namespace

struct VulkanProgramNode::Impl {
    std::mutex mutex;
    std::shared_ptr<const GpuProgram> program;
    VulkanGraphContext context{};
    std::unique_ptr<ProgramResources> resources;
};

VulkanProgramNode::VulkanProgramNode(std::shared_ptr<const GpuProgram> program,
    GraphImageContract input, GraphImageContract output)
    : VulkanNode("GPU program", 1, 1), impl_(std::make_unique<Impl>()) {
    if (!program || program->cacheId.empty() || program->shaderText.empty() || program->functionName.empty())
        throw std::invalid_argument("FilterGraph requires a complete GPU program");
    impl_->program = std::move(program);
    setImageContracts(input, output);
}
VulkanProgramNode::~VulkanProgramNode() = default;

bool VulkanProgramNode::setProgram(std::shared_ptr<const GpuProgram> program) {
    if (!program) return false;
    std::lock_guard lock(impl_->mutex);
    const auto& old = *impl_->program;
    if (program->cacheId != old.cacheId || program->uniformData.size() != old.uniformData.size()) return false;
    // cacheId owns shader/LUT/layout identity. Numeric updates do not copy or
    // compare large texture arrays. A new ID requires a new compiled graph.
    impl_->program = std::move(program);
    return true;
}

bool VulkanProgramNode::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1 || inputs[0].width <= 0 || inputs[0].height <= 0
        || inputs[0].format != inputContract().format) return false;
    const auto supported = [](FormatId format) {
        return format == toFormatId(ImageType::rgba16f) || format == toFormatId(ImageType::rgba32f);
    };
    if (!supported(inputContract().format) || !supported(outputContract().format)) return false;
    setInputFormat(0, inputs[0]);
    auto output = inputs[0]; output.format = outputContract().format;
    setOutputFormat(0, output);
    return true;
}

bool VulkanProgramNode::beginFrame(const FrameContext& frame) {
    const bool enabled = VulkanNode::beginFrame(frame);
    std::lock_guard lock(impl_->mutex);
    if (impl_->resources) {
        impl_->resources->lutUploadRecorded = false;
        // The graph has waited for the previous submission before beginFrame.
        if (impl_->resources->lutsUploaded)
            for (auto& lut : impl_->resources->luts) lut->staging.reset();
    }
    return enabled;
}

bool VulkanProgramNode::prepare(const VulkanGraphContext& context) {
    if (!context.device || !context.physicalDevice) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->resources && impl_->context.device == context.device) return true;
    impl_->resources.reset();
    impl_->context = context;
    const auto& program = *impl_->program;
    const auto& uniformData = program.uniformData;
    auto fresh = std::make_unique<ProgramResources>(context.device);
    auto& resources = *fresh;
    const char* qualifier = outputContract().format == toFormatId(ImageType::rgba32f) ? "rgba32f" : "rgba16f";
    std::string source = "#version 460\nlayout(local_size_x=8,local_size_y=8) in;\n"
        "layout(set=0,binding=0) uniform sampler2D colorInput;\nlayout(set=0,binding=1,";
    source += qualifier;
    source += ") writeonly uniform image2D colorOutput;\n";
    source += program.shaderText;
    source += "\nvoid main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(colorOutput))))return;"
              "vec4 c=texelFetch(colorInput,p,0);vec4 r=" + program.functionName + "(c);r.a=c.a;imageStore(colorOutput,p,r);}\n";
    const auto binary = compile(source);
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = binary.size() * sizeof(uint32_t); shaderInfo.pCode = binary.data();
    check(vkCreateShaderModule(context.device, &shaderInfo, nullptr, &resources.shader), "vkCreateShaderModule");
    std::vector<VkDescriptorSetLayoutBinding> mainBindings{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    std::vector<VkDescriptorSetLayoutBinding> programBindings;
    if (!program.uniformData.empty()) programBindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (const auto& lut : program.textures()) {
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
    std::vector<VkWriteDescriptorSet> writes;
    auto imageWrite = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, size_t index) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = binding; write.descriptorCount = 1;
        write.descriptorType = type; write.pImageInfo = &images[index]; writes.push_back(write);
    };
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
    impl_->resources = std::move(fresh);
    return true;
}

std::vector<LogicalResourceRequest> VulkanProgramNode::declareResourceRequests() const {
    if (outputFormats().empty()) return {};
    LogicalResourceRequest request;
    request.kind = LogicalResourceKind::GraphCreated;
    request.outputPin = 0;
    request.extent = {uint32_t(outputFormats()[0].width), uint32_t(outputFormats()[0].height)};
    request.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    request.contract = outputContract();
    return {request};
}
std::vector<ResourceAccess> VulkanProgramNode::declareResourceAccess() const {
    return {{inputResource(0), ResourceAccessMode::Read,
                {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT}},
            {logicalOutputResource(0), ResourceAccessMode::Write,
                {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT}}};
}

void VulkanProgramNode::record(VkCommandBuffer cmd, const FrameContext&) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->resources) throw std::runtime_error("GPU program node has not been prepared");
    const auto input = this->input(0), output = this->output(0);
    if (!input.valid() || !input.view || !output.valid() || !output.view
        || input.image == output.image || input.contract != inputContract()
        || output.contract != outputContract()
        || input.extent.width != output.extent.width || input.extent.height != output.extent.height
        || !(input.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(output.usage & VK_IMAGE_USAGE_STORAGE_BIT))
        throw std::runtime_error("GPU program image contract or usage mismatch");
    const auto& context = impl_->context;
    const auto& program = *impl_->program;
    auto& resources = *impl_->resources;
    const auto data = program.uniformUpdate ? program.uniformUpdate() : program.uniformData;
    if (data.size() != program.uniformData.size())
        throw std::invalid_argument("Dynamic GPU program UBO size changed");
    if (resources.ubo) resources.ubo->update(data.size(), data.data());
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
    if (!resources.lutsUploaded) {
        for (size_t i = 0; i < resources.luts.size(); ++i) {
            const auto& lut = program.textures()[i]; const auto& image = *resources.luts[i];
            barrier(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {lut.width, lut.height, lut.depth};
            vkCmdCopyBufferToImage(cmd, image.staging->buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        resources.lutUploadRecorded = !resources.luts.empty();
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.layout, 0, 2, resources.sets.data(), 0, nullptr);
    vkCmdDispatch(cmd, (output.extent.width + 7) / 8, (output.extent.height + 7) / 8, 1);
}

void VulkanProgramNode::setCompletion(VulkanSyncPoint completion) {
    VulkanNode::setCompletion(completion);
    // Keep staging memory until the graph has waited on this submission. The
    // next beginFrame runs only after that fence; teardown also drains first.
    std::lock_guard lock(impl_->mutex);
    if (active() && impl_->resources && impl_->resources->lutUploadRecorded) {
        impl_->resources->lutsUploaded = true;
        impl_->resources->lutUploadRecorded = false;
    }
}

} // namespace heisenberg::filtergraph
