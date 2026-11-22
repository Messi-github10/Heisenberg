#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <Models/BlendMode.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <MultiMedia/Video/Renderer/VulkanBlendPass.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Resources/VulkanImageResource.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
using Rgb = std::array<double, 3>;
using Pixel = std::array<float, 4>;

// This reference is deliberately independent of the production GPU source.
// The nonseparable functions follow PDF/W3C luminosity and saturation, not HSV.
double luminance(const Rgb& c) { return .3*c[0] + .59*c[1] + .11*c[2]; }
double saturation(const Rgb& c) { return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end()); }
Rgb clipColor(Rgb c) {
    const double l = luminance(c);
    const double n = *std::min_element(c.begin(), c.end());
    const double x = *std::max_element(c.begin(), c.end());
    if (n < 0) for (double& v : c) v = l + (v-l)*l/(l-n);
    if (x > 1) for (double& v : c) v = l + (v-l)*(1-l)/(x-l);
    return c;
}
Rgb withLuminance(Rgb c, double l) {
    const double offset = l-luminance(c);
    for (double& v : c) v += offset;
    return clipColor(c);
}
Rgb withSaturation(Rgb c, double s) {
    std::array<int, 3> order{0, 1, 2};
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return c[a] < c[b]; });
    const double lo = c[order[0]], hi = c[order[2]];
    if (hi > lo) {
        c[order[1]] = (c[order[1]]-lo)*s/(hi-lo);
        c[order[2]] = s;
    } else c[order[1]] = c[order[2]] = 0;
    c[order[0]] = 0;
    return c;
}
Rgb blendReference(const std::string& name, Rgb b, Rgb s) {
    const bool bounded = name == "colorBurn" || name == "colorDodge" || name == "softLight" ||
        name == "hue" || name == "saturation" || name == "color" || name == "luminosity";
    if (bounded) for (int i=0; i<3; ++i) {
        b[i] = std::clamp(b[i], 0.0, 1.0);
        s[i] = std::clamp(s[i], 0.0, 1.0);
    }
    if (name == "hue") return withLuminance(withSaturation(s, saturation(b)), luminance(b));
    if (name == "saturation") return withLuminance(withSaturation(b, saturation(s)), luminance(b));
    if (name == "color") return withLuminance(s, luminance(b));
    if (name == "luminosity") return withLuminance(b, luminance(s));
    Rgb out{};
    for (int i=0; i<3; ++i) {
        const double x=b[i], y=s[i];
        if (name == "normal") out[i]=y;
        else if (name == "darken") out[i]=std::min(x,y);
        else if (name == "multiply") out[i]=x*y;
        else if (name == "colorBurn") out[i]=x == 1 ? 1 : y == 0 ? 0 : 1-std::min(1.0,(1-x)/y);
        else if (name == "lighten") out[i]=std::max(x,y);
        else if (name == "screen") out[i]=x+y-x*y;
        else if (name == "colorDodge") out[i]=x == 0 ? 0 : y == 1 ? 1 : std::min(1.0,x/(1-y));
        else if (name == "overlay") out[i]=x <= .5 ? 2*x*y : 1-2*(1-x)*(1-y);
        else if (name == "hardLight") out[i]=y <= .5 ? 2*x*y : 1-2*(1-x)*(1-y);
        else if (name == "softLight") {
            const double d=x <= .25 ? ((16*x-12)*x+4)*x : std::sqrt(x);
            out[i]=y <= .5 ? x-(1-2*y)*x*(1-x) : x+(2*y-1)*(d-x);
        } else if (name == "difference") out[i]=std::abs(x-y);
        else if (name == "exclusion") out[i]=x+y-2*x*y;
        else if (name == "add") out[i]=x+y;
        else ADD_FAILURE() << "Reference does not recognize " << name;
    }
    return out;
}
Pixel compositeReference(const std::string& mode, Pixel b, Pixel s, double opacity) {
    const double amount=std::clamp(double(s[3]),0.0,1.0)*std::clamp(opacity,0.0,1.0);
    if (amount == 0) return {b[0],b[1],b[2],1};
    const auto mixed=blendReference(mode,{b[0],b[1],b[2]},{s[0],s[1],s[2]});
    Pixel out{0,0,0,1};
    for (int i=0; i<3; ++i) out[i]=float(b[i]*(1-amount)+mixed[i]*amount);
    return out;
}
std::shared_ptr<VideoFrame> image(int w, int h, const std::function<Pixel(int,int)>& sample) {
    auto* raw=av_frame_alloc();
    if (!raw) return {};
    auto owned=std::shared_ptr<AVFrame>(raw,[](AVFrame* p) { av_frame_free(&p); });
    raw->width=w; raw->height=h; raw->format=AV_PIX_FMT_RGBAF32;
    raw->sample_aspect_ratio={1,1}; raw->time_base={1,24};
    raw->color_range=AVCOL_RANGE_JPEG; raw->colorspace=AVCOL_SPC_RGB; raw->color_trc=AVCOL_TRC_LINEAR;
    if (av_frame_get_buffer(raw,32)<0) return {};
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        float* p=reinterpret_cast<float*>(raw->data[0]+y*raw->linesize[0])+x*4;
        const auto value=sample(x,y); std::copy(value.begin(),value.end(),p);
    }
    return VideoFrame::wrap(std::move(owned),0,Rational{24,1});
}
Pixel pixel(const AVFrame& frame,int x,int y) {
    const float* p=reinterpret_cast<const float*>(frame.data[0]+y*frame.linesize[0])+x*4;
    return {p[0],p[1],p[2],p[3]};
}
CompositionPlan plan(int w=16,int h=12) {
    CompositionPlan result;
    result.settings=TimelineSettings::fromTimelineSettings(w,h,24); result.frameCount=24;
    for (const auto* id : {"upper", "lower"}) {
        CompositionClip clip; clip.id=id; clip.mediaRef=id; clip.durationFrames=24;
        clip.visualSnapshot.id=id; clip.visualSnapshot.mediaRef=id; clip.visualSnapshot.durationFrames=24;
        result.tracks.push_back({std::string(id)+"-track",ClipType::Video,false,false,{clip}});
        result.colorSettings.inputColorSpaces[id]=color::kWorkingSpace;
    }
    return result;
}

class BlendRendererTest : public testing::Test {
protected:
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<CompositionRenderer> renderer;
    std::string error;
    bool initialized=false;
    uint64_t validationBaseline=0;
    void SetUp() override {
        auto& vk=VulkanContext::instance();
        try {
            vk.initLoader(); vk.createInstance(true); vk.createDevice();
            initialized=true; validationBaseline=vk.validationErrorCount();
            VulkanResources r;
            r.instance=vk.vkInstance(); r.physDevice=vk.physicalDevice(); r.device=vk.device();
            r.graphicsQF=vk.graphicsQueueFamily(); r.graphicsQueue=vk.graphicsQueue(); r.getProcAddr=vk.getInstanceProcAddr();
            r.enabledFeatures=vk.enabledDeviceFeatures(); r.enabledExtensions=vk.enabledDeviceExtensions();
            gpu=std::make_unique<GpuContext>(r);
        } catch (const std::exception& e) { GTEST_SKIP() << e.what(); }
        renderer=std::make_unique<CompositionRenderer>();
        ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    }
    void TearDown() override {
        renderer.reset(); gpu.reset();
        if (initialized) EXPECT_EQ(VulkanContext::instance().validationErrorCount(),validationBaseline);
    }
    RenderedFrame render(const CompositionPlan& p,std::shared_ptr<VideoFrame> upper,
                         std::shared_ptr<VideoFrame> lower,double opacity=1) {
        std::vector<CompositionVideoLayer> layers{
            {upper,p.tracks[0].id,p.tracks[0].clips[0].id,0,opacity},
            {lower,p.tracks[1].id,p.tracks[1].clips[0].id,0,1}};
        return renderer->render(p,0,layers,&error);
    }
    std::shared_ptr<AVFrame> download(const RenderedFrame& frame) {
        EXPECT_TRUE(frame) << error;
        return frame ? renderer->download(frame,DownloadFormat::RgbaFloat,&error) : nullptr;
    }
    void expect(Pixel actual,Pixel expected,float tolerance=.006f) {
        for (int c=0;c<4;++c) {
            EXPECT_TRUE(std::isfinite(actual[c]));
            EXPECT_NEAR(actual[c],expected[c],tolerance) << "component " << c;
        }
    }
};

TEST_F(BlendRendererTest, AllSeventeenModesMatchIndependentReferenceAcrossColorsAlphaAndOpacity) {
    const std::array<Pixel,8> sources{{{0,1,.5f,1},{1,0,.25f,1},{.2f,.7f,.9f,.5f},
        {.75f,.25f,.5f,.25f},{.5f,.5f,.5f,1},{.9f,.1f,.6f,0},{.2f,.7f,.9f,1},{.8f,.4f,.1f,.75f}}};
    const std::array<Pixel,8> backdrops{{{0,1,.25f,1},{1,0,.75f,1},{.8f,.3f,.1f,1},
        {.25f,.75f,.5f,1},{.5f,.5f,.5f,1},{.1f,.6f,.4f,1},{.2f,.2f,.2f,1},{.7f,.2f,.9f,1}}};
    auto upper=image(16,12,[&](int x,int) {return sources[x%8];});
    auto lower=image(16,12,[&](int x,int) {return backdrops[x%8];});
    ASSERT_TRUE(upper && lower);
    for (const auto& descriptor : blendModes()) for (const double opacity : {0.0,.37,1.0}) {
        const std::string name(descriptor.name);
        SCOPED_TRACE(testing::Message() << name << " opacity " << opacity);
        auto p=plan(); p.tracks[0].clips[0].visualSnapshot.blendMode=name;
        auto frame=render(p,upper,lower,opacity); auto actual=download(frame); ASSERT_TRUE(actual) << error;
        for (int x=0;x<16;++x) expect(pixel(*actual,x,6),compositeReference(name,backdrops[x%8],sources[x%8],opacity));
    }
}

TEST_F(BlendRendererTest, HdrSignedColorsUseDocumentedPerModeDomainWithoutGlobalClamping) {
    const Pixel s{-0.5f,2,.375f,.5f}, b{1.5f,-.25f,3,1};
    auto upper=image(16,12,[&](int,int){return s;});
    auto lower=image(16,12,[&](int,int){return b;});
    for (const auto& descriptor : blendModes()) {
        const std::string name(descriptor.name); SCOPED_TRACE(name);
        auto p=plan(); p.tracks[0].clips[0].visualSnapshot.blendMode=name;
        auto actual=download(render(p,upper,lower,.6)); ASSERT_TRUE(actual) << error;
        expect(pixel(*actual,8,6),compositeReference(name,b,s,.6),.012f);
    }
}

TEST_F(BlendRendererTest, TransparentSingularEndpointsLeaveBackdropUntouched) {
    const Pixel b{0,1,.3f,1};
    auto lower=image(16,12,[&](int,int){return b;});
    auto hidden=image(16,12,[](int,int){return Pixel{1,0,8192,0};});
    for (const auto& descriptor : blendModes()) {
        SCOPED_TRACE(std::string(descriptor.name));
        auto p=plan(); p.tracks[0].clips[0].visualSnapshot.blendMode=std::string(descriptor.name);
        auto actual=download(render(p,hidden,lower)); ASSERT_TRUE(actual) << error;
        expect(pixel(*actual,8,6),b);
    }
}

TEST_F(BlendRendererTest, MaskedRotatedGeometryPreservesBackgroundAndAllModeCoverage) {
    const Pixel s{.8f,.25f,.55f,1}, b{.2f,.6f,.35f,1};
    auto white=image(16,12,[](int,int){return Pixel{1,1,1,1};});
    auto black=image(24,20,[](int,int){return Pixel{0,0,0,1};});
    auto upper=image(16,12,[&](int,int){return s;});
    auto lower=image(24,20,[&](int,int){return b;});
    auto p=plan(24,20);
    auto& appearance=p.tracks[0].clips[0].visualSnapshot;
    appearance.transform.width=.8; appearance.transform.height=.9; appearance.transform.rotation=27;
    appearance.transform.centerX=.6; appearance.transform.flipHorizontal=true;
    appearance.crop={.2,.1,.05,.1}; appearance.edgeRounding=.6; appearance.edgeSoftness=.4;
    auto coverageImage=download(render(p,white,black)); ASSERT_TRUE(coverageImage) << error;
    for (const auto& descriptor : blendModes()) {
        const std::string name(descriptor.name); SCOPED_TRACE(name); appearance.blendMode=name;
        auto actual=download(render(p,upper,lower,.65)); ASSERT_TRUE(actual) << error;
        for (int y=0;y<20;y+=2) for (int x=0;x<24;x+=2) {
            auto sampled=s; sampled[3]=pixel(*coverageImage,x,y)[0];
            expect(pixel(*actual,x,y),compositeReference(name,b,sampled,.65),.01f);
        }
    }
}

TEST_F(BlendRendererTest, ClipGradePrecedesBlendAndGlobalGradeFollowsComposite) {
    const Pixel s{.25f,.375f,.125f,.5f}, b{.4f,.2f,.8f,1};
    auto upper=image(16,12,[&](int,int){return s;});
    auto lower=image(16,12,[&](int,int){return b;});
    const auto exposure=JsonValue::parse(R"([{"type":"video.colorGrade","schemaVersion":1,"params":{"exposure":{"value":1}}}])");
    for (const auto* mode : {"multiply","screen","overlay","hue","add"}) {
        SCOPED_TRACE(mode);
        auto p=plan();
        p.tracks[0].clips[0].visualSnapshot.effects=exposure;
        p.tracks[0].clips[0].visualSnapshot.blendMode=mode;
        auto actual=download(render(p,upper,lower,.7)); ASSERT_TRUE(actual) << error;
        auto graded=s; for (int c=0;c<3;++c) graded[c]*=2;
        const auto composite=compositeReference(mode,b,graded,.7);
        expect(pixel(*actual,8,6),composite,.008f);
    }
    Clip global;
    global.effects=JsonValue::parse(R"([{"type":"video.colorGrade","schemaVersion":1,"params":{"contrast":{"value":1.2}}}])");
    grade::CompiledGrade compiled; color::ColorPipeline pipeline;
    ASSERT_TRUE(grade::GradeCompiler::compile(global,0,compiled,&error,{},&pipeline)) << error;
    ASSERT_TRUE(renderer->setFilterGraphSnapshot(compiled.document,&error)) << error;
    auto p=plan(); p.tracks[0].clips[0].visualSnapshot.blendMode="screen";
    auto actual=download(render(p,upper,lower,.7)); ASSERT_TRUE(actual) << error;
    ASSERT_TRUE(renderer->setFilterGraphSnapshot({},&error)) << error;
    const auto mixed=compositeReference("screen",b,s,.7);
    auto reference=image(16,12,[&](int,int){return mixed;});
    p=plan(); p.tracks[0].clips[0].visualSnapshot.effects=global.effects;
    auto expected=download(render(p,reference,lower)); ASSERT_TRUE(expected) << error;
    expect(pixel(*actual,8,6),pixel(*expected,8,6),.008f);
}

TEST_F(BlendRendererTest, ModeChangesResizeAndShutdownNeverOverwritePublishedFrames) {
    const Pixel s{.75f,.25f,.5f,.5f}, b{.25f,.75f,.5f,1};
    auto upper=image(16,12,[&](int,int){return s;});
    auto lower=image(16,12,[&](int,int){return b;});
    auto p=plan(); p.tracks[0].clips[0].visualSnapshot.blendMode="multiply";
    auto held=render(p,upper,lower,.75); ASSERT_TRUE(held) << error;
    auto original=download(held); ASSERT_TRUE(original);
    for (const auto& descriptor : blendModes()) {
        p.tracks[0].clips[0].visualSnapshot.blendMode=std::string(descriptor.name);
        auto current=download(render(p,upper,lower)); ASSERT_TRUE(current) << error;
    }
    p.settings=TimelineSettings::fromTimelineSettings(32,24,24);
    auto resized=download(render(p,upper,lower)); ASSERT_TRUE(resized);
    auto after=download(held); ASSERT_TRUE(after);
    for (int y=0;y<12;++y) for (int x=0;x<16;++x) EXPECT_EQ(pixel(*after,x,y),pixel(*original,x,y));
    renderer->shutdown();
    ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    auto survived=download(held); ASSERT_TRUE(survived) << error;
    expect(pixel(*survived,8,6),compositeReference("multiply",b,s,.75));
}

TEST_F(BlendRendererTest, OneNativePipelineHandlesEveryModeAndCancellationWithoutRecompilation) {
    auto p=plan();
    auto lower=image(16,12,[](int,int){return Pixel{.2f,.3f,.4f,1};});
    auto upper=image(16,12,[](int,int){return Pixel{.7f,.4f,.1f,.5f};});
    auto input=render(p,upper,lower); ASSERT_TRUE(input) << error;
    auto source=render(p,lower,upper); ASSERT_TRUE(source) << error;
    auto& vk=VulkanContext::instance();
    filtergraph::VulkanGraphContext context{};
    context.instance=vk.vkInstance(); context.physicalDevice=vk.physicalDevice(); context.device=vk.device();
    context.queue=vk.graphicsQueue(); context.queueFamilyIndex=vk.graphicsQueueFamily();
    filtergraph::VulkanImageResource output(context);
    ASSERT_TRUE(output.ensure({16,12},VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,filtergraph::kWorkingImageContract));
    VulkanBlendPass pass(context);
    for (const auto& descriptor : blendModes()) {
        VulkanBlendParameters parameters; parameters.mode=descriptor.mode; parameters.opacity=.65f;
        const auto ready=pass.execute(input.image(),source.image(),output.ref(),parameters);
        ASSERT_TRUE(ready.valid()); output.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const auto stats=pass.cacheStats();
    EXPECT_EQ(stats.shaderCompilations,1u); EXPECT_EQ(stats.pipelineCreations,1u);
    EXPECT_EQ(stats.dispatches,blendModes().size());
    EXPECT_THROW(pass.execute(input.image(),source.image(),input.image()),std::invalid_argument);
    VulkanBlendParameters invalid; invalid.mode=static_cast<BlendMode>(999);
    EXPECT_THROW(pass.execute(input.image(),source.image(),output.ref(),invalid),std::invalid_argument);
    pass.setCancellation([] {return true;});
    EXPECT_THROW(pass.execute(input.image(),source.image(),output.ref()),std::runtime_error);
    EXPECT_EQ(pass.cacheStats().dispatches,stats.dispatches);
    int checks=0;
    pass.setCancellation([&] {return ++checks >= 3;});
    EXPECT_THROW(pass.execute(input.image(),source.image(),output.ref()),std::runtime_error);
    EXPECT_GE(checks,3);
    EXPECT_EQ(pass.cacheStats().dispatches,stats.dispatches+1);
    pass.setCancellation({});
    EXPECT_TRUE(pass.execute(input.image(),source.image(),output.ref()).valid());
    EXPECT_EQ(pass.cacheStats().shaderCompilations,1u);
}

TEST_F(BlendRendererTest, UnknownModeRejectsPreviewStrictHiddenAndZeroOpacityPlans) {
    auto p=plan(); auto source=image(16,12,[](int,int){return Pixel{.5f,.2f,.1f,1};}); ASSERT_TRUE(source);
    for (const auto* unknown : {"unknown", "", "Multiply"}) for (const bool strict : {false,true}) {
        p.tracks[0].clips[0].visualSnapshot.blendMode=unknown;
        for (const bool hidden : {false,true}) {
            p.tracks[0].hidden=hidden;
            CompositionRenderRequest request;
            request.settings=p.settings; request.plan=&p; request.strict=strict;
            request.layers={{source,p.tracks[0].id,p.tracks[0].clips[0].id,0,0}};
            error.clear();
            auto rejected=renderer->render(request,&error);
            EXPECT_FALSE(rejected) << unknown << " strict=" << strict << " hidden=" << hidden;
            EXPECT_NE(error.find("blend"),std::string::npos) << error;
        }
    }
}
} // namespace
