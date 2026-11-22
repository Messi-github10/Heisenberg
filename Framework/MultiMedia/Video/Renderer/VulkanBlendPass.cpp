#include <volk.h>
#include "VulkanBlendPass.hpp"
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <shaderc/shaderc.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace heisenberg::renderer {
namespace {
using namespace filtergraph;
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(int(result)) + ")");
}
// The single shader uses the shared enum as a uniform, never as generated
// source. Pin the ABI here so edits to the model cannot silently change math.
static_assert(int(BlendMode::Normal) == 0 && int(BlendMode::Darken) == 1
    && int(BlendMode::Multiply) == 2 && int(BlendMode::ColorBurn) == 3
    && int(BlendMode::Lighten) == 4 && int(BlendMode::Screen) == 5
    && int(BlendMode::ColorDodge) == 6 && int(BlendMode::Overlay) == 7
    && int(BlendMode::SoftLight) == 8 && int(BlendMode::HardLight) == 9
    && int(BlendMode::Difference) == 10 && int(BlendMode::Exclusion) == 11
    && int(BlendMode::Hue) == 12 && int(BlendMode::Saturation) == 13
    && int(BlendMode::Color) == 14 && int(BlendMode::Luminosity) == 15
    && int(BlendMode::Add) == 16);
constexpr char blendShader[] = R"glsl(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0) uniform sampler2D backdrop;
layout(set=0,binding=1) uniform sampler2D source;
layout(set=0,binding=2,rgba16f) writeonly uniform image2D destination;
layout(push_constant) uniform Parameters {
    vec4 sourceX;
    vec4 sourceY;
    float opacity;
    int mode;
    int affine;
    int premultiplied;
} params;
float lum(vec3 c) { return dot(c,vec3(0.3,0.59,0.11)); }
float sat(vec3 c) { return max(max(c.r,c.g),c.b)-min(min(c.r,c.g),c.b); }
vec3 clipColor(vec3 c) {
    float l=lum(c), n=min(min(c.r,c.g),c.b), x=max(max(c.r,c.g),c.b);
    if(n<0.0) c=vec3(l)+(c-vec3(l))*l/(l-n);
    if(x>1.0) c=vec3(l)+(c-vec3(l))*(1.0-l)/(x-l);
    return c;
}
vec3 setLum(vec3 c,float l) { return clipColor(c+vec3(l-lum(c))); }
vec3 setSat(vec3 c,float s) {
    float n=min(min(c.r,c.g),c.b), x=max(max(c.r,c.g),c.b);
    if(x>n) return (c-vec3(n))*s/(x-n);
    return vec3(0.0);
}
float burn(float b,float s) {
    if(b==1.0) return 1.0;
    if(s==0.0) return 0.0;
    return 1.0-min(1.0,(1.0-b)/s);
}
float dodge(float b,float s) {
    if(b==0.0) return 0.0;
    if(s==1.0) return 1.0;
    return min(1.0,b/(1.0-s));
}
float softLight(float b,float s) {
    if(s<=0.5) return b-(1.0-2.0*s)*b*(1.0-b);
    float d;
    if(b<=0.25) d=((16.0*b-12.0)*b+4.0)*b;
    else d=sqrt(b);
    return b+(2.0*s-1.0)*(d-b);
}
vec3 hardLight(vec3 b,vec3 s) {
    vec3 lo=2.0*b*s, hi=vec3(1.0)-2.0*(vec3(1.0)-b)*(vec3(1.0)-s);
    return mix(hi,lo,lessThanEqual(s,vec3(0.5)));
}
vec3 blend(vec3 b,vec3 s) {
    // Only these modes have a bounded operand domain. The backdrop used in
    // compositing below stays untouched, including negative and HDR values.
    if(params.mode==3 || params.mode==6 || params.mode==8 ||
       (params.mode>=12 && params.mode<=15)) {
        b=clamp(b,0.0,1.0); s=clamp(s,0.0,1.0);
    }
    switch(params.mode) {
        case 0: return s;
        case 1: return min(b,s);
        case 2: return b*s;
        case 3: return vec3(burn(b.r,s.r),burn(b.g,s.g),burn(b.b,s.b));
        case 4: return max(b,s);
        case 5: return b+s-b*s;
        case 6: return vec3(dodge(b.r,s.r),dodge(b.g,s.g),dodge(b.b,s.b));
        case 7: return hardLight(s,b);
        case 8: return vec3(softLight(b.r,s.r),softLight(b.g,s.g),softLight(b.b,s.b));
        case 9: return hardLight(b,s);
        case 10: return abs(b-s);
        case 11: return b+s-2.0*b*s;
        case 12: return setLum(setSat(s,sat(b)),lum(b));
        case 13: return setLum(setSat(b,sat(s)),lum(b));
        case 14: return setLum(s,lum(b));
        case 15: return setLum(b,lum(s));
        case 16: return b+s;
    }
    return vec3(uintBitsToFloat(0x7fc00000u)); // Unreachable: CPU rejects unknown modes.
}
vec4 sampleSource(ivec2 p) {
    if(params.affine==0) return texelFetch(source,p,0);
    vec3 canvas=vec3((vec2(p)+0.5)/vec2(imageSize(destination)),1.0);
    vec2 uv=vec2(dot(params.sourceX.xyz,canvas),dot(params.sourceY.xyz,canvas));
    ivec2 size=textureSize(source,0);
    vec2 position=uv*vec2(size)-0.5;
    vec4 color=vec4(0.0);
    // Reject before float-to-int conversion, including extreme offscreen UVs.
    if(all(greaterThan(position,vec2(-1.0))) && all(lessThan(position,vec2(size)))) {
        ivec2 base=ivec2(floor(position)); vec2 f=fract(position);
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 tap=base+ivec2(x,y);
            if(all(greaterThanEqual(tap,ivec2(0))) && all(lessThan(tap,size))) {
                float weight=(x==0?1.0-f.x:f.x)*(y==0?1.0-f.y:f.y);
                if(weight>0.0) {
                    vec4 value=texelFetch(source,tap,0);
                    if(params.premultiplied==0) {
                        value.a=clamp(value.a,0.0,1.0);
                        value.rgb=value.a>0.0?value.rgb*value.a:vec3(0.0);
                    }
                    color+=value*weight;
                }
            }
        }
    }
    return color;
}
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(p,imageSize(destination)))) return;
    vec3 b=texelFetch(backdrop,p,0).rgb;
    if(params.opacity==0.0) { imageStore(destination,p,vec4(b,1.0)); return; }
    vec4 s=sampleSource(p);
    float amount=clamp(s.a,0.0,1.0)*params.opacity;
    // Avoid evaluating hidden RGB, unpremultiplication or blend formulas when
    // coverage is zero. Multiplying a NaN by zero would still pollute b.
    if(amount==0.0) { imageStore(destination,p,vec4(b,1.0)); return; }
    if(params.premultiplied!=0 || params.affine!=0) s.rgb/=s.a;
    // B*(1-o)+(B*(1-a)+F(B,S)*a)*o, with the same straight-alpha
    // expression for Normal. No global clamp on input or output RGB.
    vec3 result=b*(1.0-amount)+blend(b,s.rgb)*amount;
    imageStore(destination,p,vec4(result,1.0));
}
)glsl";
std::vector<uint32_t> compileShader() {
    auto compiler = shaderc_compiler_initialize();
    auto options = shaderc_compile_options_initialize();
    if (!compiler || !options) {
        if (options) shaderc_compile_options_release(options);
        if (compiler) shaderc_compiler_release(compiler);
        throw std::runtime_error("Cannot initialize blend shader compiler");
    }
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_5);
    auto result = shaderc_compile_into_spv(compiler, blendShader, sizeof(blendShader)-1,
        shaderc_compute_shader, "VulkanBlendPass.comp", "main", options);
    std::string error;
    std::vector<uint32_t> binary;
    if (!result) error = "shaderc returned no result";
    else if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
        error = shaderc_result_get_error_message(result);
    else {
        binary.resize(shaderc_result_get_length(result)/sizeof(uint32_t));
        std::memcpy(binary.data(),shaderc_result_get_bytes(result),binary.size()*sizeof(uint32_t));
    }
    if (result) shaderc_result_release(result);
    shaderc_compile_options_release(options); shaderc_compiler_release(compiler);
    if (!error.empty()) throw std::runtime_error("Vulkan blend shader compile failed: " + error);
    return binary;
}
void barrier(VkCommandBuffer cmd, const VulkanImageRef& image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout=from; b.newLayout=to;
    b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,nullptr,0,nullptr,1,&b);
}
struct PushConstants {
    std::array<float,4> sourceX, sourceY;
    float opacity;
    int32_t mode, affine, premultiplied;
};
static_assert(sizeof(PushConstants)==48);
struct Resources {
    VkDevice device;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    bool submitted=false;
    explicit Resources(VkDevice d) : device(d) {}
    void drain() {
        if (submitted) { vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX); submitted=false; }
    }
    ~Resources() {
        drain();
        if (fence) vkDestroyFence(device,fence,nullptr);
        if (pool) vkDestroyCommandPool(device,pool,nullptr);
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if (descriptors) vkDestroyDescriptorPool(device,descriptors,nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        if (sampler) vkDestroySampler(device,sampler,nullptr);
        if (shader) vkDestroyShaderModule(device,shader,nullptr);
    }
};
}
struct VulkanBlendPass::Impl {
    VulkanGraphContext context;
    VkSemaphore timeline=VK_NULL_HANDLE;
    uint64_t value=0;
    std::unique_ptr<Resources> resources;
    std::function<bool()> cancellation;
    mutable std::mutex mutex;
    VulkanBlendCacheStats stats;
    explicit Impl(VulkanGraphContext c) : context(c) {
        if (!c.device || !c.physicalDevice || !c.queue || c.queueFamilyIndex==VK_QUEUE_FAMILY_IGNORED)
            throw std::invalid_argument("VulkanBlendPass needs a complete VulkanGraphContext");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext=&type;
        check(vkCreateSemaphore(c.device,&info,nullptr,&timeline),"vkCreateSemaphore(blend)");
    }
    ~Impl() { resources.reset(); if (timeline) vkDestroySemaphore(context.device,timeline,nullptr); }
    void checkCancellation() const {
        if (cancellation && cancellation()) throw std::runtime_error("Rendering cancelled");
    }
    void initialize() {
        if (resources) return;
        auto next=std::make_unique<Resources>(context.device);
        auto& r=*next;
        const auto binary=compileShader(); ++stats.shaderCompilations;
        checkCancellation();
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize=binary.size()*sizeof(uint32_t); shader.pCode=binary.data();
        check(vkCreateShaderModule(context.device,&shader,nullptr,&r.shader),"vkCreateShaderModule(blend)");
        const std::array<VkDescriptorSetLayoutBinding,3> bindings{{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount=uint32_t(bindings.size()); set.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(context.device,&set,nullptr,&r.setLayout),"vkCreateDescriptorSetLayout(blend)");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount=1; layout.pSetLayouts=&r.setLayout; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(context.device,&layout,nullptr,&r.layout),"vkCreatePipelineLayout(blend)");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module=r.shader; pipeline.stage.pName="main"; pipeline.layout=r.layout;
        check(vkCreateComputePipelines(context.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline),"vkCreateComputePipelines(blend)");
        ++stats.pipelineCreations;
        const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets=1; pool.poolSizeCount=uint32_t(sizes.size()); pool.pPoolSizes=sizes.data();
        check(vkCreateDescriptorPool(context.device,&pool,nullptr,&r.descriptors),"vkCreateDescriptorPool(blend)");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=r.descriptors; allocate.descriptorSetCount=1; allocate.pSetLayouts=&r.setLayout;
        check(vkAllocateDescriptorSets(context.device,&allocate,&r.set),"vkAllocateDescriptorSets(blend)");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST; sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(context.device,&sampler,nullptr,&r.sampler),"vkCreateSampler(blend)");
        VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; commandPool.queueFamilyIndex=context.queueFamilyIndex;
        check(vkCreateCommandPool(context.device,&commandPool,nullptr,&r.pool),"vkCreateCommandPool(blend)");
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool=r.pool; command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount=1;
        check(vkAllocateCommandBuffers(context.device,&command,&r.command),"vkAllocateCommandBuffers(blend)");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device,&fence,nullptr,&r.fence),"vkCreateFence(blend)");
        resources=std::move(next);
    }
};
VulkanBlendPass::VulkanBlendPass(VulkanGraphContext c) : impl_(std::make_unique<Impl>(c)) {}
VulkanBlendPass::~VulkanBlendPass()=default;
void VulkanBlendPass::setCancellation(std::function<bool()> cancellation) {
    std::lock_guard lock(impl_->mutex); impl_->cancellation=std::move(cancellation);
}
VulkanBlendCacheStats VulkanBlendPass::cacheStats() const {
    std::lock_guard lock(impl_->mutex); return impl_->stats;
}
VulkanSyncPoint VulkanBlendPass::execute(const VulkanImageRef& backdrop, const VulkanImageRef& source,
    const VulkanImageRef& output, const VulkanBlendParameters& parameters) {
    std::lock_guard lock(impl_->mutex);
    impl_->checkCancellation();
    const auto& context=impl_->context;
    if (!backdrop.valid() || !source.valid() || !output.valid() || !backdrop.view || !source.view || !output.view
        || output.image==backdrop.image || output.image==source.image
        || backdrop.extent.width!=output.extent.width || backdrop.extent.height!=output.extent.height
        || (!parameters.affine && (source.extent.width!=output.extent.width || source.extent.height!=output.extent.height))
        || !(backdrop.usage&VK_IMAGE_USAGE_SAMPLED_BIT) || !(source.usage&VK_IMAGE_USAGE_SAMPLED_BIT)
        || !(output.usage&VK_IMAGE_USAGE_STORAGE_BIT) || backdrop.layout==VK_IMAGE_LAYOUT_UNDEFINED || source.layout==VK_IMAGE_LAYOUT_UNDEFINED)
        throw std::invalid_argument("VulkanBlendPass needs sampled inputs and a distinct canvas-sized storage output");
    for (const auto* image : {&backdrop,&source,&output}) {
        if (image->vkFormat!=VK_FORMAT_R16G16B16A16_SFLOAT)
            throw std::invalid_argument("VulkanBlendPass requires RGBA16F images");
        if (image->queueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED && image->queueFamilyIndex!=context.queueFamilyIndex)
            throw std::invalid_argument("VulkanBlendPass caller must transfer image queue ownership");
    }
    auto sourceContract=source.contract;
    const bool supportedSourceAlpha=sourceContract.alpha==AlphaMode::straight || sourceContract.alpha==AlphaMode::premultiplied;
    sourceContract.alpha=AlphaMode::straight;
    if (backdrop.contract!=kWorkingImageContract || output.contract!=kWorkingImageContract
        || !supportedSourceAlpha || sourceContract!=kWorkingImageContract)
        throw std::invalid_argument("VulkanBlendPass requires scene-linear AP1 straight-alpha working images (premultiplied source preparation allowed)");
    const int mode=int(parameters.mode);
    if (mode<int(BlendMode::Normal) || mode>int(BlendMode::Add) || !std::isfinite(parameters.opacity)
        || parameters.opacity<0 || parameters.opacity>1)
        throw std::invalid_argument("Invalid blend mode or opacity");
    for (const auto& row : {parameters.sourceX,parameters.sourceY})
        for (float value : row) if (!std::isfinite(value)) throw std::invalid_argument("Invalid blend sampling affine");
    if (backdrop.image==source.image && backdrop.layout!=source.layout)
        throw std::invalid_argument("Aliased blend inputs must use the same layout");
    impl_->initialize(); auto& r=*impl_->resources;
    // A previous call always completed or drained before descriptors/commands
    // can be rewritten. This also makes resize and shutdown deterministic.
    r.drain();
    check(vkResetCommandPool(context.device,r.pool,0),"vkResetCommandPool(blend)");
    check(vkResetFences(context.device,1,&r.fence),"vkResetFences(blend)");
    const std::array<VkDescriptorImageInfo,3> images{{
        {r.sampler,backdrop.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {r.sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet,3> writes{};
    for (uint32_t i=0;i<writes.size();++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=r.set; writes[i].dstBinding=i;
        writes[i].descriptorCount=1; writes[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo=&images[i];
    }
    vkUpdateDescriptorSets(context.device,uint32_t(writes.size()),writes.data(),0,nullptr);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(r.command,&begin),"vkBeginCommandBuffer(blend)");
    barrier(r.command,backdrop,backdrop.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (source.image!=backdrop.image) barrier(r.command,source,source.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(r.command,output,output.layout,VK_IMAGE_LAYOUT_GENERAL);
    const PushConstants push{parameters.sourceX,parameters.sourceY,parameters.opacity,mode,parameters.affine?1:0,
        source.contract.alpha==AlphaMode::premultiplied?1:0};
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&r.set,0,nullptr);
    vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(r.command,(output.extent.width+7)/8,(output.extent.height+7)/8,1);
    barrier(r.command,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(r.command,backdrop,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,backdrop.layout);
    if (source.image!=backdrop.image) barrier(r.command,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,source.layout);
    check(vkEndCommandBuffer(r.command),"vkEndCommandBuffer(blend)");
    std::vector<VkSemaphore> waits; std::vector<uint64_t> values;
    for (const auto& ready : {backdrop.ready,source.ready,output.ready}) {
        if (!ready.valid()) continue;
        const auto found=std::find(waits.begin(),waits.end(),ready.semaphore);
        if (found==waits.end()) { waits.push_back(ready.semaphore); values.push_back(ready.value); }
        else values[size_t(found-waits.begin())]=std::max(values[size_t(found-waits.begin())],ready.value);
    }
    const std::vector<VkPipelineStageFlags> stages(waits.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const uint64_t signal=++impl_->value;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount=uint32_t(values.size()); timeline.pWaitSemaphoreValues=values.data();
    timeline.signalSemaphoreValueCount=1; timeline.pSignalSemaphoreValues=&signal;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&timeline;
    submit.waitSemaphoreCount=uint32_t(waits.size()); submit.pWaitSemaphores=waits.data(); submit.pWaitDstStageMask=stages.data();
    submit.commandBufferCount=1; submit.pCommandBuffers=&r.command; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&impl_->timeline;
    impl_->checkCancellation();
    check(synchronizedQueueSubmit(context.queue,1,&submit,r.fence),"vkQueueSubmit(blend)"); r.submitted=true;
    struct Drain { Resources& resources; ~Drain() { resources.drain(); } } drain{r};
    ++impl_->stats.dispatches;
    bool cancelled=false;
    for (;;) {
        if (impl_->cancellation && impl_->cancellation()) cancelled=true;
        const auto status=vkWaitForFences(context.device,1,&r.fence,VK_TRUE,10'000'000);
        if (status==VK_TIMEOUT) continue;
        check(status,"vkWaitForFences(blend)"); break;
    }
    r.submitted=false;
    if (impl_->cancellation && impl_->cancellation()) cancelled=true;
    if (cancelled) throw std::runtime_error("Rendering cancelled");
    return {impl_->timeline,signal};
}
} // namespace heisenberg::renderer
