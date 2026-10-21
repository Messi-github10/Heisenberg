#include <volk.h>
#include <Compositing/Grade/ColorGrade.hpp>
#include <Compositing/Grade/GradeGpuPrograms.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace heisenberg;
using namespace heisenberg::grade;
using namespace heisenberg::filtergraph;
namespace color=heisenberg::renderer::color;
using Domain=ColorGradeProgramDomain;
using Pixel=std::array<float,4>;
JsonValue effect() { return {{"type",kColorGradeType},{"schemaVersion",1},{"params",JsonValue::object()}}; }
Pixel apply(const ColorGradeState& s,Domain d,Pixel p) { return referenceColorGradePixel(s,d,p); }
float y709(const Pixel& p) { return p[0]*0.2126f+p[1]*0.7152f+p[2]*0.0722f; }
float chroma(const Pixel& p) { return std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]}); }
void expectPixel(const Pixel& actual,const Pixel& expected,float tolerance=1e-6f) {
    for(size_t c=0;c<4;++c) EXPECT_NEAR(actual[c],expected[c],tolerance*std::max(1.0f,std::abs(expected[c])))<<"channel "<<c;
}
void checked(VkResult result) { if(result!=VK_SUCCESS) throw std::runtime_error("color grade Vulkan operation failed: "+std::to_string(int(result))); }
class ColorGradeGpuTest : public testing::Test {
protected:
    VulkanGraphContext context{};
    VkCommandPool pool=VK_NULL_HANDLE;
    uint64_t validationBaseline=0;
    void SetUp() override {
        try {
            auto& vk=renderer::VulkanContext::instance();
            vk.initLoader();vk.createInstance(false);vk.createDevice();
            context.instance=vk.vkInstance();context.physicalDevice=vk.physicalDevice();context.device=vk.device();
            context.queue=vk.graphicsQueue();context.queueFamilyIndex=vk.graphicsQueueFamily();
            validationBaseline=vk.validationErrorCount();
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};info.queueFamilyIndex=context.queueFamilyIndex;
            checked(vkCreateCommandPool(context.device,&info,nullptr,&pool));
        } catch(const std::exception& e) { GTEST_SKIP()<<"No windowless Vulkan GPU: "<<e.what(); }
    }
    void TearDown() override {
        if(pool) vkDestroyCommandPool(context.device,pool,nullptr);
        if(context.device) EXPECT_EQ(renderer::VulkanContext::instance().validationErrorCount(),validationBaseline);
    }
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(context.physicalDevice,&props);
        for(uint32_t i=0;i<props.memoryTypeCount;++i) if((bits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("No Vulkan memory type for grade test");
    }
    struct Image {
        VkDevice device;VulkanImageRef ref{};VkDeviceMemory memory=VK_NULL_HANDLE;
        explicit Image(VkDevice d):device(d){}
        ~Image() { if(ref.view) vkDestroyImageView(device,ref.view,nullptr);if(ref.image) vkDestroyImage(device,ref.image,nullptr);if(memory) vkFreeMemory(device,memory,nullptr); }
    };
    std::unique_ptr<Image> image(uint32_t width) {
        auto result=std::make_unique<Image>(context.device);auto& ref=result->ref;
        ref.extent={width,1};ref.vkFormat=VK_FORMAT_R32G32B32A32_SFLOAT;ref.queueFamilyIndex=context.queueFamilyIndex;
        ref.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.extent={width,1,1};
        info.format=ref.vkFormat;info.mipLevels=1;info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=ref.usage;
        checked(vkCreateImage(context.device,&info,nullptr,&ref.image));
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(context.device,ref.image,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(context.device,&alloc,nullptr,&result->memory));checked(vkBindImageMemory(context.device,ref.image,result->memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=ref.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=ref.vkFormat;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        checked(vkCreateImageView(context.device,&view,nullptr,&ref.view));return result;
    }
    void transfer(Image& image,std::vector<Pixel>& pixels,bool upload) {
        VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=pixels.size()*sizeof(Pixel);info.usage=upload?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateBuffer(context.device,&info,nullptr,&buffer));
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(context.device,buffer,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checked(vkAllocateMemory(context.device,&alloc,nullptr,&memory));checked(vkBindBufferMemory(context.device,buffer,memory,0));
        void* mapped=nullptr;checked(vkMapMemory(context.device,memory,0,info.size,0,&mapped));if(upload) std::memcpy(mapped,pixels.data(),size_t(info.size));
        VkCommandBuffer command;VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cmd.commandPool=pool;cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
        checked(vkAllocateCommandBuffers(context.device,&cmd,&command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};checked(vkBeginCommandBuffer(command,&begin));
        auto transition=[&](VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags source,VkAccessFlags destination){
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image.ref.image;barrier.oldLayout=oldLayout;barrier.newLayout=newLayout;
            barrier.srcAccessMask=source;barrier.dstAccessMask=destination;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        };
        const auto layout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        transition(image.ref.layout,layout,image.ref.layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={image.ref.extent.width,1,1};
        if(upload) vkCmdCopyBufferToImage(command,buffer,image.ref.image,layout,1,&copy);else vkCmdCopyImageToBuffer(command,image.ref.image,layout,buffer,1,&copy);
        transition(layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT);
        checked(vkEndCommandBuffer(command));VkFence fence;VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checked(vkCreateFence(context.device,&fi,nullptr,&fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        checked(renderer::synchronizedQueueSubmit(context.queue,1,&submit,fence));checked(vkWaitForFences(context.device,1,&fence,VK_TRUE,UINT64_MAX));
        if(!upload) std::memcpy(pixels.data(),mapped,size_t(info.size));
        vkUnmapMemory(context.device,memory);vkDestroyFence(context.device,fence,nullptr);vkFreeCommandBuffers(context.device,pool,1,&command);vkDestroyBuffer(context.device,buffer,nullptr);vkFreeMemory(context.device,memory,nullptr);
        image.ref.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    std::vector<Pixel> execute(const color::GpuProgram& program,const std::vector<Pixel>& input,color::ColorGpuPass& pass) {
        auto in=image(uint32_t(input.size())),out=image(uint32_t(input.size()));auto pixels=input;transfer(*in,pixels,true);
        auto ready=pass.execute(program,in->ref,out->ref);EXPECT_TRUE(ready.valid());out->ref.ready=ready;out->ref.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transfer(*out,pixels,false);return pixels;
    }
    void compare(const ColorGradeState& state,Domain domain,const std::vector<Pixel>& input,float tolerance=1e-5f) {
        color::ColorGpuPass pass(context);const auto output=execute(makeColorGradeGpuProgram(state,domain),input,pass);
        for(size_t i=0;i<input.size();++i) {
            SCOPED_TRACE(i);const auto expected=apply(state,domain,input[i]);
            for(float n:output[i]) EXPECT_TRUE(std::isfinite(n));
            expectPixel(output[i],expected,tolerance);EXPECT_FLOAT_EQ(output[i][3],input[i][3]);
        }
    }
};
} // namespace

TEST(ColorGradeStateTest, VersionDefaultsAnimationAndTransactionalValidation) {
    auto json=effect();ColorGradeState state;std::vector<GradeDiagnostic> diagnostics;std::string error;
    ASSERT_TRUE(sampleColorGrade(json,0,state,diagnostics,&error))<<error;EXPECT_EQ(state,ColorGradeState{});
    json["params"]["exposure"]={{"value",0},{"track",{{"keyframes",JsonValue::array({{{"frame",0},{"value",-2}},{{"frame",10},{"value",2}}})}}}};
    ASSERT_TRUE(sampleColorGrade(json,5,state,diagnostics,&error));EXPECT_FLOAT_EQ(state.ev,0);
    ASSERT_TRUE(sampleColorGrade(json,10,state,diagnostics,&error));EXPECT_FLOAT_EQ(state.ev,2);
    auto prior=state;json["params"]["exposure"]["track"]["keyframes"][1]["value"]=4;
    EXPECT_FALSE(sampleColorGrade(json,-100,state,diagnostics,&error));EXPECT_EQ(state,prior);EXPECT_FALSE(error.empty());
    json=effect();json.erase("schemaVersion");EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
    json=effect();json["schemaVersion"]=2;EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
    json=effect();json["params"]["contrast"]={{"value",std::numeric_limits<double>::infinity()}};EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
    json=effect();json["params"]["exposure"]={{"value",1e-100}};EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
}

TEST(ColorGradeStateTest, RegistryEndpointsAndAllHiddenKeysValidated) {
    for(const auto& spec:colorGradeParameterRegistry()) {
        auto json=effect();json["params"][spec.name]={{"value",spec.minimum}};
        ColorGradeState state;std::vector<GradeDiagnostic> diagnostics;std::string error;
        EXPECT_TRUE(sampleColorGrade(json,0,state,diagnostics,&error))<<spec.name<<error;
        json["params"][spec.name]["value"]=spec.maximum;EXPECT_TRUE(sampleColorGrade(json,0,state,diagnostics,&error))<<spec.name<<error;
        json["params"][spec.name]["value"]=double(spec.maximum)+1;EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics))<<spec.name;
    }
    auto json=effect();json["params"]["gainMaster"]={{"value",1},{"track",{{"keyframes",JsonValue::array({{{"frame",100},{"value",0.1}}})}}}};
    ColorGradeState state;std::vector<GradeDiagnostic> diagnostics;EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
}

TEST(ColorGradeStateTest, CanonicalizationPreservesUnknownFieldsAndAnimatedNeutralBases) {
    auto json=effect();json["future"]={{"data",7}};json["params"]["futureScalar"]={{"value",9}};
    json["params"]["exposure"]={{"value",0},{"editor","metadata"},{"track",{{"keyframes",JsonValue::array({{{"frame",0},{"value",0}},{{"frame",10},{"value",2}}})}}}};
    json["params"]["curves"]={{"master",{{0,0},{0.25,0.25},{1,1}}},{"red",{{0.2,0.2},{0.8,0.8}}},{"futureCurve","opaque"}};
    const auto original=json;JsonValue normalized;std::string error;
    ASSERT_TRUE(canonicalizeColorGrade(json,normalized,&error))<<error;EXPECT_EQ(json,original);
    EXPECT_EQ(normalized["future"],original["future"]);EXPECT_EQ(normalized["params"]["futureScalar"],original["params"]["futureScalar"]);
    EXPECT_EQ(normalized["params"]["exposure"],original["params"]["exposure"]);
    EXPECT_FALSE(normalized["params"]["curves"].contains("master"));EXPECT_TRUE(normalized["params"]["curves"].contains("red"));
    EXPECT_EQ(normalized["params"]["curves"]["futureCurve"],"opaque");EXPECT_EQ(normalized["params"]["gammaMaster"]["value"],1);
    JsonValue again;ASSERT_TRUE(canonicalizeColorGrade(normalized,again,&error));EXPECT_EQ(normalized,again);
}

TEST(ColorGradeStateTest, ExactCurveRepresentationAndArbitraryAnimatedHueTargets) {
    auto json=effect();json["params"]["hueTargets"]=JsonValue::array({{{"hue",355},{"rotation",{{"track",{{"keyframes",JsonValue::array({{{"frame",0},{"value",-20}},{{"frame",10},{"value",20}}})}}}}},{"saturation",1.3},{"lightness",-0.2}}});
    ColorGradeState state;std::vector<GradeDiagnostic> diagnostics;std::string error;
    ASSERT_TRUE(sampleColorGrade(json,5,state,diagnostics,&error))<<error;ASSERT_EQ(state.hueTargets.size(),1u);EXPECT_FLOAT_EQ(state.hueTargets[0].hue,355);EXPECT_FLOAT_EQ(state.hueTargets[0].rotation,0);
    json["params"]["curves"]={{"red",{{0,0},{0.5,0.7},{0.50000000001,0.8},{1,1}}}};
    EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics,&error));
    json["params"].erase("curves");json["params"]["hueTargets"][0]["rotation"]=31;EXPECT_FALSE(sampleColorGrade(json,0,state,diagnostics));
}

TEST(ColorGradeProgramTest, DomainsNeutralOmissionAndUniformCacheIdentity) {
    std::vector<ColorGradeGpuStage> stages;ColorGradeState s;std::string error;
    ASSERT_TRUE(buildColorGradeGpuPrograms(s,stages,&error));EXPECT_TRUE(stages.empty());
    s.ev=1;s.contrast=1.2f;s.saturation=0.8f;
    ASSERT_TRUE(buildColorGradeGpuPrograms(s,stages,&error));ASSERT_EQ(stages.size(),3u);
    EXPECT_EQ(stages[0].domain,"ACEScg");EXPECT_EQ(stages[1].domain,"ACEScct");EXPECT_EQ(stages[2].domain,kColorGradeAuxiliarySpace);
    auto old=stages;s.ev=2;s.contrast=0.6f;s.hueTargets={{355,10,1.2f,0.1f}};
    ASSERT_TRUE(buildColorGradeGpuPrograms(s,stages,&error));
    for(size_t i=0;i<3;++i) {EXPECT_EQ(old[i].program.cacheId,stages[i].program.cacheId);EXPECT_EQ(old[i].program.shaderText,stages[i].program.shaderText);EXPECT_NE(old[i].program.uniformData,stages[i].program.uniformData);}
    const auto previous=stages[2].program.cacheId;s.curves.red={{0,0},{0.3f,0.6f},{1,1}};
    ASSERT_TRUE(buildColorGradeGpuPrograms(s,stages,&error));EXPECT_NE(previous,stages[2].program.cacheId);
    EXPECT_FALSE(stages[2].program.luts[0].linearInterpolation);EXPECT_EQ(stages[2].program.luts[0].width,64u);
    s.enabled=false;ASSERT_TRUE(buildColorGradeGpuPrograms(s,stages));EXPECT_TRUE(stages.empty());
}

TEST(ColorGradeMathTest, DefaultIdentityExposureAndCalibratedWhiteBalance) {
    const Pixel p{-0.01f,0.18f,2.0f,-0.3f};ColorGradeState s;
    for(auto d:{Domain::LinearACEScg,Domain::ToneACEScct,Domain::Auxiliary}) expectPixel(apply(s,d,p),p);
    s.ev=2;expectPixel(apply(s,Domain::LinearACEScg,p),{-0.04f,0.72f,8,-0.3f});
    s.ev=0;const Pixel neutral{0.18f,0.18f,0.18f,1};
    s.temperature=11000;const auto warm=apply(s,Domain::LinearACEScg,neutral);EXPECT_GT(warm[0],warm[2]);
    s.temperature=2000;const auto cool=apply(s,Domain::LinearACEScg,neutral);EXPECT_LT(cool[0],cool[2]);
    s.temperature=6500;s.tint=100;const auto green=apply(s,Domain::LinearACEScg,neutral);EXPECT_GT(green[1],(green[0]+green[2])/2);
    s.tint=-100;const auto magenta=apply(s,Domain::LinearACEScg,neutral);EXPECT_LT(magenta[1],(magenta[0]+magenta[2])/2);
    for(float t:{2000.0f,6500.0f,11000.0f}) for(float tint:{-100.0f,0.0f,100.0f}) {
        s.temperature=t;s.tint=tint;const auto out=apply(s,Domain::LinearACEScg,neutral);
        for(float c:out) EXPECT_TRUE(std::isfinite(c));
        EXPECT_NEAR(out[0]*0.2722287168f+out[1]*0.6740817658f+out[2]*0.0536895174f,0.18f,2e-6f);
    }
}

TEST(ColorGradeMathTest, ACEScctToneMasksAndInverseGammaNearBlack) {
    constexpr float black=0.0729055342f,white=0.5547945205f,pivot=0.4135884025f;ColorGradeState s;
    s.blacks=1;EXPECT_NEAR(apply(s,Domain::ToneACEScct,{black,black,black,1})[0],black+0.08f,1e-6f);
    EXPECT_NEAR(apply(s,Domain::ToneACEScct,{white,white,white,1})[0],white,1e-6f);
    s.blacks=0;s.whites=1;EXPECT_NEAR(apply(s,Domain::ToneACEScct,{white,white,white,1})[0],white+0.08f,1e-6f);
    EXPECT_NEAR(apply(s,Domain::ToneACEScct,{black,black,black,1})[0],black,1e-6f);
    s.whites=0;s.contrast=1.5f;expectPixel(apply(s,Domain::ToneACEScct,{pivot,pivot,pivot,1}),{pivot,pivot,pivot,1});
    s=ColorGradeState{};s.gamma.master=2;
    expectPixel(apply(s,Domain::Auxiliary,{-1e-8f,0,0.25f,0.37f}),{-1e-4f,0,0.5f,0.37f},2e-6f);
    s.gamma.master=1;s.lift.master=0.1f;EXPECT_NEAR(apply(s,Domain::Auxiliary,{0,0,0,1})[0],0.1f,1e-6f);
    s.lift.master=0;s.gain.master=1.5f;expectPixel(apply(s,Domain::Auxiliary,{-0.1f,1,2,1}),{-0.15f,1.5f,3,1});
}

TEST(ColorGradeMathTest, PresenceWheelDirectionsAndExactPiecewiseCurves) {
    const Pixel p{0.6f,0.4f,0.3f,1};ColorGradeState s;s.saturation=0;
    const auto gray=apply(s,Domain::Auxiliary,p);EXPECT_NEAR(gray[0],y709(p),1e-6f);EXPECT_NEAR(gray[0],gray[2],1e-6f);
    s.saturation=1;s.vibrance=1;EXPECT_GT(chroma(apply(s,Domain::Auxiliary,p)),chroma(p));
    s.vibrance=-1;EXPECT_NEAR(chroma(apply(s,Domain::Auxiliary,p)),0,1e-6f);
    s=ColorGradeState{};s.lift.strength=1;s.lift.hue=0;const auto red=apply(s,Domain::Auxiliary,{0.2f,0.2f,0.2f,1});EXPECT_GT(red[0],red[1]);EXPECT_GT(red[0],red[2]);
    s=ColorGradeState{};s.curves.master={{0.1f,0.2f},{0.4f,0.8f},{0.9f,0.6f}};
    s.curves.red={{0,0},{1,0.5f}};expectPixel(apply(s,Domain::Auxiliary,{0.25f,0.25f,0.25f,0.2f}),{0.25f,0.5f,0.5f,0.2f});
}

TEST(ColorGradeMathTest, MasterCurveMapsRec709LuminanceAndPreservesChromaticityOutsideToe) {
    ColorGradeState state;state.curves.master={{0,0},{0.5f,0.25f},{1,1}};
    const Pixel source{0.8f,0.3f,0.1f,0.37f};
    const float luminance=y709(source);ASSERT_LT(luminance,0.5f);
    const auto mapped=apply(state,Domain::Auxiliary,source);
    // The first segment is Y'=Y/2, so every channel must have the same scale.
    expectPixel(mapped,{0.4f,0.15f,0.05f,0.37f});
    EXPECT_NEAR(y709(mapped),luminance/2,1e-6f);
    EXPECT_NEAR(mapped[0]*source[1],mapped[1]*source[0],1e-6f);
    EXPECT_NEAR(mapped[0]*source[2],mapped[2]*source[0],1e-6f);
    state.curves.master={{0,0.1f},{1,1}};
    expectPixel(apply(state,Domain::Auxiliary,{0,0,0,0.23f}),{0.1f,0.1f,0.1f,0.23f});
    // Endpoint-flat applies to negative luminance and HDR, not odd extension.
    EXPECT_NEAR(y709(apply(state,Domain::Auxiliary,{-0.1f,-0.2f,-0.3f,1})),0.1f,1e-6f);
    EXPECT_NEAR(y709(apply(state,Domain::Auxiliary,{2,3,4,1})),1,1e-6f);
    for(float sign:{-1.0f,1.0f}) {
        const float a=sign*1e-8f;
        const auto toe=apply(state,Domain::Auxiliary,{3*a,a,a,0.23f});
        for(float value:toe) EXPECT_TRUE(std::isfinite(value));
        EXPECT_NEAR(toe[0],0.1f,1e-4f);EXPECT_NEAR(toe[1],0.1f,1e-4f);EXPECT_NEAR(toe[2],0.1f,1e-4f);
    }
    const Pixel negativeHdr{-0.2f,2,4,0.4f};state.curves.master={{0,0},{0.2f,0.2f},{1,1}};
    expectPixel(apply(state,Domain::Auxiliary,negativeHdr),negativeHdr);
    state.curves.master.clear();expectPixel(apply(state,Domain::Auxiliary,negativeHdr),negativeHdr);
}

TEST(ColorGradeMathTest, CyclicHueFrozenQueriesOverlapReorderingAndGrayGate) {
    ColorGradeState s;s.hueTargets={{355,20,1,0}};
    const auto red=apply(s,Domain::Auxiliary,{1,0,0,1});EXPECT_GT(red[1]-red[2],0.1f);EXPECT_NEAR(y709(red),0.2126f,1e-6f);
    expectPixel(apply(s,Domain::Auxiliary,{0.3f,0.3f,0.3f,0.4f}),{0.3f,0.3f,0.3f,0.4f});
    expectPixel(apply(s,Domain::Auxiliary,{0.300001f,0.3f,0.3f,0.4f}),{0.300001f,0.3f,0.3f,0.4f});
    s.hueTargets={{0,30,1,0},{30,0,0,0.5f}};
    const auto frozen=apply(s,Domain::Auxiliary,{1,0,0,1});EXPECT_NEAR(chroma(frozen),1,1e-6f);EXPECT_NEAR(y709(frozen),0.2126f,1e-6f);
    s.hueTargets={{355,20,1.3f,0.1f},{5,-10,0.8f,-0.05f},{0,8,1.4f,0.02f}};
    const auto program=makeColorGradeGpuProgram(s,Domain::Auxiliary);const auto first=apply(s,Domain::Auxiliary,{1,0,0,1});
    std::reverse(s.hueTargets.begin(),s.hueTargets.end());EXPECT_EQ(program.uniformData,makeColorGradeGpuProgram(s,Domain::Auxiliary).uniformData);EXPECT_EQ(first,apply(s,Domain::Auxiliary,{1,0,0,1}));
    s.hueTargets={{0,0,1,0.2f}};EXPECT_NEAR(y709(apply(s,Domain::Auxiliary,{1,0,0,1})),0.4126f,1e-6f);
}

TEST_F(ColorGradeGpuTest, AllDomainsMatchReferenceWithNegativesHDRAndAlpha) {
    const std::vector<Pixel> pixels={{-0.1f,0,0.001f,-0.5f},{1e-8f,0.18f,0.5f,0},{1,2,4,0.3f},{0.18f,0.18f,0.18f,1},{0.001f,-0.001f,0.002f,0.8f}};
    ColorGradeState s;
    for(auto d:{Domain::LinearACEScg,Domain::ToneACEScct,Domain::Auxiliary}) compare(s,d,pixels);
    s.ev=2.5f;s.temperature=11000;s.tint=70;compare(s,Domain::LinearACEScg,pixels);
    s.temperature=2000;s.tint=-100;compare(s,Domain::LinearACEScg,pixels);
    s.contrast=1.3f;s.highlights=0.7f;s.shadows=-0.6f;s.blacks=0.4f;s.whites=-0.3f;compare(s,Domain::ToneACEScct,pixels);
    s.gamma.master=2;compare(s,Domain::Auxiliary,pixels);
    s.lift={25,0.7f,0.1f};s.gamma={250,0.6f,1.4f};s.gain={130,0.4f,1.2f};s.vibrance=0.8f;s.saturation=1.2f;
    compare(s,Domain::Auxiliary,pixels,3e-5f);
}

TEST_F(ColorGradeGpuTest, MasterLuminanceCurveChromaticityBlackToeAndEndpointExtrapolation) {
    ColorGradeState state;state.curves.master={{0,0},{0.5f,0.25f},{1,1}};
    const std::vector<Pixel> samples={{0.8f,0.3f,0.1f,0.37f},{0,0,0,0.23f},
        {3e-8f,1e-8f,1e-8f,0.4f},{-3e-8f,-1e-8f,-1e-8f,0.6f},
        {-0.1f,-0.2f,-0.3f,0.8f},{2,3,4,1},{-0.2f,2,4,-0.3f}};
    compare(state,Domain::Auxiliary,samples,4e-6f);
    color::ColorGpuPass pass(context);
    const auto result=execute(makeColorGradeGpuProgram(state,Domain::Auxiliary),samples,pass);
    expectPixel(result[0],{0.4f,0.15f,0.05f,0.37f},4e-6f);
    state.curves.master={{0,0.1f},{1,1}};
    compare(state,Domain::Auxiliary,samples,4e-6f);
    const auto lifted=execute(makeColorGradeGpuProgram(state,Domain::Auxiliary),samples,pass);
    expectPixel(lifted[1],{0.1f,0.1f,0.1f,0.23f},4e-6f);
    state.curves.master={{0,0},{1,1}};
    const auto identity=execute(makeColorGradeGpuProgram(state,Domain::Auxiliary),samples,pass);
    for(size_t i=0;i<samples.size();++i) expectPixel(identity[i],samples[i],4e-6f);
}

TEST_F(ColorGradeGpuTest, Exact64PointCurvesCyclicTargetsAndUniformResourceReuse) {
    ColorGradeState s;
    for(int i=0;i<64;++i) {const float x=float(i)/63;s.curves.master.push_back({x,x*x});}
    s.curves.red={{0.1f,0.2f},{0.3212345f,0.8f},{0.9f,0.6f}};
    std::vector<Pixel> pixels;
    for(int i=0;i<127;++i) {const float x=float(i)/126;pixels.push_back({x,x,x,0.37f});}
    pixels.push_back({-0.1f,2,4,-0.5f});compare(s,Domain::Auxiliary,pixels,3e-6f);
    s=ColorGradeState{};s.hueTargets={{355,25,1.3f,0.1f},{5,-10,0.5f,-0.02f},{120,-20,1.5f,0.2f}};
    pixels={{1,0,0,0.2f},{1,0.01f,0,0.4f},{1,0,0.01f,0.6f},{0.3f,0.3f,0.3f,0.8f},{0.3001f,0.3f,0.3f,1},{0,1,0,-0.5f},{-0.1f,2,0.2f,0.1f}};
    compare(s,Domain::Auxiliary,pixels,2e-5f);
    color::ColorGpuPass pass(context);const auto a=makeColorGradeGpuProgram(s,Domain::Auxiliary);execute(a,pixels,pass);
    s.saturation=0.4f;s.hueTargets[0].rotation=-20;const auto b=makeColorGradeGpuProgram(s,Domain::Auxiliary);const auto out=execute(b,pixels,pass);
    EXPECT_EQ(a.cacheId,b.cacheId);EXPECT_EQ(pass.cacheStats().shaderCompilations,1u);EXPECT_EQ(pass.cacheStats().lutUploads,1u);EXPECT_EQ(pass.cacheStats().hits,1u);
    for(size_t i=0;i<pixels.size();++i) expectPixel(out[i],apply(s,Domain::Auxiliary,pixels[i]),2e-5f);
}
