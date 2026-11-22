#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <Models/ClipAppearance.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
using Pixel = std::array<float,4>;

struct Pattern {
    int width, height;
    AVRational sar{1,1};
    std::vector<Pixel> pixels;
    std::shared_ptr<VideoFrame> frame;
    Pattern(int w, int h, const std::function<Pixel(int,int)>& sample, AVRational aspect = {1,1})
        : width(w), height(h), sar(aspect) {
        auto* raw = av_frame_alloc();
        if (!raw) return;
        auto owned = std::shared_ptr<AVFrame>(raw,[](AVFrame* p) { av_frame_free(&p); });
        raw->width = w; raw->height = h; raw->format = AV_PIX_FMT_RGBAF32;
        raw->sample_aspect_ratio = aspect; raw->time_base = {1,24};
        raw->color_range = AVCOL_RANGE_JPEG; raw->colorspace = AVCOL_SPC_RGB;
        raw->color_trc = AVCOL_TRC_LINEAR;
        if (av_frame_get_buffer(raw,32) < 0) return;
        for (int y=0; y<h; ++y) {
            auto* row = reinterpret_cast<float*>(raw->data[0]+y*raw->linesize[0]);
            for (int x=0; x<w; ++x) {
                const auto p = sample(x,y); pixels.push_back(p);
                std::copy(p.begin(),p.end(),row+x*4);
            }
        }
        frame = VideoFrame::wrap(std::move(owned),0,Rational{24,1});
    }
};

Pixel asymmetric(int x,int y) {
    // Distinct corners, no axial symmetry, negative/HDR RGB and hidden color.
    const float alpha = (x/7+y/5)%5 == 0 ? 0.0f : (x+2*y)%4 == 0 ? 0.35f : 1.0f;
    return {x < 13 ? 2.0f : 0.15f, y < 11 ? -0.25f : 0.7f,
            float((x*3+y*7)%19)/18.0f,alpha};
}

CompositionPlan makePlan(int width,int height,const Clip& appearance = {}) {
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(width,height,24);
    plan.frameCount = 24;
    CompositionClip clip; clip.id = "geometry"; clip.mediaRef = "pattern"; clip.durationFrames = 24;
    clip.visualSnapshot = appearance;
    clip.visualSnapshot.id = clip.id; clip.visualSnapshot.mediaRef = clip.mediaRef;
    clip.visualSnapshot.durationFrames = 24;
    CompositionTrack track; track.id = "video"; track.clips.push_back(clip);
    plan.tracks.push_back(track);
    plan.colorSettings.inputColorSpaces[clip.mediaRef] = color::kWorkingSpace;
    return plan;
}

Pixel pixel(const AVFrame& image,int x,int y) {
    const float* p = reinterpret_cast<const float*>(image.data[0]+y*image.linesize[0])+x*4;
    return {p[0],p[1],p[2],p[3]};
}

// Independent source-pixel mask reference. Its distance is measured before
// SAR, fitting, scale, reflection or rotation, as required by the model.
double coverage(const Pattern& source,const Clip& clip,int x,int y) {
    const double left = clip.crop.left*source.width;
    const double right = (1-clip.crop.right)*source.width;
    const double top = clip.crop.top*source.height;
    const double bottom = (1-clip.crop.bottom)*source.height;
    const double radius = clip.edgeRounding*std::min(right-left,bottom-top)/2;
    const double feather = clip.edgeSoftness*std::min(right-left,bottom-top)/2;
    const double qx = std::abs(x+0.5-(left+right)/2)-(right-left)/2+radius;
    const double qy = std::abs(y+0.5-(top+bottom)/2)-(bottom-top)/2+radius;
    const double outside = std::hypot(std::max(qx,0.0),std::max(qy,0.0));
    const double inside = std::min(std::max(qx,qy),0.0);
    const double distance = outside+inside-radius;
    const double t = std::clamp((distance+0.5+feather)/(1+feather),0.0,1.0);
    return 1-t*t*(3-2*t);
}

Pixel reference(const Pattern& source,const Clip& clip,int w,int h,int x,int y) {
    const double aspect = source.width*av_q2d(source.sar)/source.height;
    const double fittedW = std::min(double(w),h*aspect)*clip.transform.width;
    const double fittedH = std::min(double(h),w/aspect)*clip.transform.height;
    const double radians = clip.transform.rotation*3.14159265358979323846/180.0;
    const double dx = x+0.5-w*clip.transform.centerX;
    const double dy = y+0.5-h*clip.transform.centerY;
    double u = (std::cos(radians)*dx+std::sin(radians)*dy)/fittedW;
    double v = (-std::sin(radians)*dx+std::cos(radians)*dy)/fittedH;
    if (clip.transform.flipHorizontal) u = -u;
    if (clip.transform.flipVertical) v = -v;
    const double sx = (u+0.5)*source.width-0.5;
    const double sy = (v+0.5)*source.height-0.5;
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    const double fx = sx-ix, fy = sy-iy;
    Pixel result{0,0,0,1};
    for (int j=0; j<2; ++j) for (int i=0; i<2; ++i) {
        const int xx = ix+i, yy = iy+j;
        if (xx<0 || yy<0 || xx>=source.width || yy>=source.height) continue;
        const auto p = source.pixels[size_t(yy)*source.width+xx];
        const double weight = (i ? fx : 1-fx)*(j ? fy : 1-fy);
        const double alpha = std::clamp(double(p[3]),0.0,1.0)*coverage(source,clip,xx,yy);
        for (int c=0; c<3; ++c) result[c] += float(p[c]*alpha*weight*clip.opacity);
    }
    return result;
}

class CompositionGeometryRendererTest : public testing::Test {
protected:
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<CompositionRenderer> renderer;
    std::string error;
    uint64_t validationBaseline = 0;
    bool contextReady = false;
    void SetUp() override {
        auto& context = VulkanContext::instance();
        try {
            context.initLoader(); context.createInstance(true); context.createDevice();
            validationBaseline = context.validationErrorCount(); contextReady = true;
            VulkanResources resources;
            resources.instance = context.vkInstance(); resources.physDevice = context.physicalDevice();
            resources.device = context.device(); resources.graphicsQF = context.graphicsQueueFamily();
            resources.graphicsQueue = context.graphicsQueue(); resources.getProcAddr = context.getInstanceProcAddr();
            resources.enabledFeatures = context.enabledDeviceFeatures();
            resources.enabledExtensions = context.enabledDeviceExtensions();
            gpu = std::make_unique<GpuContext>(resources);
        } catch (const std::exception& e) { GTEST_SKIP() << "No Vulkan GPU: " << e.what(); }
        renderer = std::make_unique<CompositionRenderer>();
        ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    }
    void TearDown() override {
        renderer.reset(); gpu.reset();
        if (contextReady) EXPECT_EQ(VulkanContext::instance().validationErrorCount(),validationBaseline);
    }
    std::shared_ptr<AVFrame> render(const Pattern& source,const Clip& clip,int w,int h) {
        auto plan = makePlan(w,h,clip);
        auto result = renderer->render(plan,0,source.frame,&error);
        if (!result) { ADD_FAILURE() << error; return {}; }
        EXPECT_EQ(result.image().contract,filtergraph::kWorkingImageContract);
        EXPECT_FALSE(result.encoded());
        return renderer->download(result,DownloadFormat::RgbaFloat,&error);
    }
    void checkReference(const Pattern& source,const Clip& clip,int w,int h,float tolerance = 0.004f) {
        auto image = render(source,clip,w,h); ASSERT_TRUE(image) << error;
        // Every pixel, including transparent bounds and all interpolation edges.
        for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
            const auto expected = reference(source,clip,w,h,x,y), actual = pixel(*image,x,y);
            for (int c=0; c<4; ++c)
                ASSERT_NEAR(actual[c],expected[c],tolerance) << "x=" << x << " y=" << y << " c=" << c;
        }
    }
};

TEST_F(CompositionGeometryRendererTest, DefaultsAndSarFitPreserveAsymmetricPixels) {
    Pattern squarePixels(48,32,asymmetric); ASSERT_TRUE(squarePixels.frame);
    checkReference(squarePixels,{},48,32); // exact identity fast path
    checkReference(squarePixels,{},96,80); // letterbox + alpha-aware magnification
    Pattern anamorphic(48,32,asymmetric,{2,1}); ASSERT_TRUE(anamorphic.frame);
    checkReference(anamorphic,{},192,96);
    Pattern invalidSar(48,32,asymmetric,{0,1}); ASSERT_TRUE(invalidSar.frame);
    checkReference(squarePixels,{},96,80);
    auto actual = render(invalidSar,{},96,80); auto expected = render(squarePixels,{},96,80);
    ASSERT_TRUE(actual); ASSERT_TRUE(expected);
    for (int y=0;y<80;++y) for (int x=0;x<96;++x)
        EXPECT_EQ(pixel(*actual,x,y),pixel(*expected,x,y));
}

TEST_F(CompositionGeometryRendererTest, ClockwiseRotationLocalFlipsAndIndependentScaleUseFullImagePivot) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    for (const double angle : {0.0,90.0,-90.0,31.0,450.0}) {
        for (const auto flips : {std::array{false,false},std::array{true,false},
                                std::array{false,true},std::array{true,true}}) {
            SCOPED_TRACE(testing::Message() << angle << " flips=" << flips[0] << flips[1]);
            Clip clip; clip.transform.centerX = 0.63; clip.transform.centerY = 0.37;
            clip.transform.width = 0.8; clip.transform.height = 1.2;
            clip.transform.rotation = angle;
            clip.transform.flipHorizontal = flips[0]; clip.transform.flipVertical = flips[1];
            clip.opacity = 0.65;
            checkReference(source,clip,96,80);
        }
    }
}

TEST_F(CompositionGeometryRendererTest, CropBeforeFlipsDoesNotZoomOrRecenterAndEdgesUseCroppedShortSide) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    for (const double round : {0.0,0.7,1.0}) for (const double soft : {0.0,0.4,1.0}) {
        SCOPED_TRACE(testing::Message() << "round=" << round << " soft=" << soft);
        Clip clip; clip.crop = {0.25,0.125,0.0625,0.25};
        clip.edgeRounding = round; clip.edgeSoftness = soft;
        clip.transform.rotation = 23; clip.transform.width = 1.25; clip.transform.height = 0.75;
        clip.transform.flipHorizontal = true; clip.transform.centerX = 0.45;
        checkReference(source,clip,96,80);
    }
}

TEST_F(CompositionGeometryRendererTest, SourcePreviewIgnoresTimelineGeometryButRetainsGrade) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    Clip clip; clip.crop = {0.25,0.2,0.3,0.1}; clip.edgeRounding = 0.8; clip.edgeSoftness = 0.5;
    clip.transform.centerX = -0.2; clip.transform.rotation = 67; clip.transform.width = 0.4;
    clip.opacity = 0.2;
    clip.effects = JsonValue::parse(R"([{"type":"video.exposure","params":{"exposure":{"value":1}}}])");
    auto plan = makePlan(96,80,clip);
    CompositionRenderRequest request; request.plan = &plan; request.source = source.frame;
    request.clipId = "geometry"; request.sourceMode = true;
    auto result = renderer->render(request,&error); ASSERT_TRUE(result) << error;
    auto image = renderer->download(result,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(image) << error;
    ASSERT_EQ(image->width,48); ASSERT_EQ(image->height,32);
    for (int y=0;y<32;++y) for (int x=0;x<48;++x) {
        const auto actual = pixel(*image,x,y), p = source.pixels[size_t(y)*48+x];
        for (int c=0;c<3;++c) EXPECT_NEAR(actual[c],2*p[c]*p[3],0.004f);
        EXPECT_FLOAT_EQ(actual[3],1.0f);
    }
}

TEST_F(CompositionGeometryRendererTest, SourcePreviewRetainsSarFitWhileIgnoringClipGeometry) {
    Pattern source(48,32,[](int x,int y) { return Pixel{x<20 ? 0.8f : 0.2f,y<13 ? 0.1f : 0.6f,0.3f,1}; },{2,1});
    ASSERT_TRUE(source.frame);
    Clip clip; clip.crop = {0.2,0.2,0.3,0.1}; clip.edgeSoftness = 0.8;
    clip.transform.rotation = 83; clip.transform.width = 0.4; clip.transform.centerX = -1;
    auto plan = makePlan(96,80,clip);
    CompositionRenderRequest request; request.plan = &plan; request.source = source.frame;
    request.clipId = "geometry"; request.sourceMode = true;
    auto preview = renderer->render(request,&error); ASSERT_TRUE(preview) << error;
    auto actual = renderer->download(preview,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(actual);
    auto expected = render(source,{},48,32); ASSERT_TRUE(expected);
    EXPECT_EQ(actual->width,48); EXPECT_EQ(actual->height,32);
    EXPECT_FLOAT_EQ(pixel(*actual,24,0)[0],0.0f);
    EXPECT_FLOAT_EQ(pixel(*actual,24,31)[0],0.0f);
    EXPECT_GT(pixel(*actual,12,16)[0],0.5f);
    for (int y=0;y<32;++y) for (int x=0;x<48;++x)
        EXPECT_EQ(pixel(*actual,x,y),pixel(*expected,x,y));
}

TEST_F(CompositionGeometryRendererTest, GradeRunsBeforeMaskAndGlobalGraphRunsAfterComposite) {
    Pattern source(48,32,[](int,int) { return Pixel{0.18f,0.4f,-0.05f,0.5f}; });
    Clip clip; clip.crop = {0.25,0.0,0.0,0.0}; clip.edgeSoftness = 0.5; clip.opacity = 0.7;
    clip.effects = JsonValue::parse(R"([{"type":"video.exposure","params":{"exposure":{"value":2}}}])");
    auto actual = render(source,clip,96,80); ASSERT_TRUE(actual) << error;
    Pattern graded(48,32,[](int,int) { return Pixel{0.72f,1.6f,-0.2f,0.5f}; });
    for (int y=0;y<80;++y) for (int x=0;x<96;++x) {
        auto expected = reference(graded,clip,96,80,x,y); auto p = pixel(*actual,x,y);
        for (int c=0;c<4;++c) ASSERT_NEAR(p[c],expected[c],0.004f) << x << ',' << y << ',' << c;
    }
    Clip global;
    global.effects = JsonValue::parse(R"([{"type":"video.basicGrade","params":{"contrast":{"value":1.2}}}])");
    grade::CompiledGrade compiled; color::ColorPipeline pipeline;
    ASSERT_TRUE(grade::GradeCompiler::compile(global,0,compiled,&error,{},&pipeline)) << error;
    ASSERT_TRUE(renderer->setFilterGraphSnapshot(compiled.document,&error)) << error;
    auto filtered = render(source,clip,96,80); ASSERT_TRUE(filtered) << error;
    ASSERT_TRUE(renderer->setFilterGraphSnapshot({},&error));
    Pattern composite(96,80,[&](int x,int y) { return pixel(*actual,x,y); });
    auto expected = render(composite,global,96,80); ASSERT_TRUE(expected) << error;
    for (int y=0;y<80;++y) for (int x=0;x<96;++x) {
        auto a = pixel(*filtered,x,y), b = pixel(*expected,x,y);
        for (int c=0;c<4;++c) ASSERT_NEAR(a[c],b[c],0.006f) << x << ',' << y << ',' << c;
    }
}

TEST_F(CompositionGeometryRendererTest, PremultipliedMinificationRejectsHiddenHdrRgbAndPreservesArea) {
    // The subpixel impulses disappear under a four-tap-only 8:1 resize. A
    // proper low-pass reconstructs their area, including partly transparent RGB.
    Pattern source(256,128,[](int x,int y) {
        const bool lit = x%8 == 1 && y%8 == 1;
        return lit ? Pixel{8.0f,-2.0f,4.0f,0.5f} : Pixel{1000.0f,-1000.0f,500.0f,0.0f};
    }); ASSERT_TRUE(source.frame);
    Clip clip;
    auto image = render(source,clip,32,16); ASSERT_TRUE(image) << error;
    Pixel mean{}; int count = 0;
    for (int y=3;y<13;++y) for (int x=3;x<29;++x) {
        const auto p = pixel(*image,x,y); ++count;
        for (int c=0;c<4;++c) mean[c] += p[c];
        EXPECT_NEAR(p[0],0.0625f,0.012f);
        EXPECT_NEAR(p[1],-0.015625f,0.003f);
        EXPECT_NEAR(p[2],0.03125f,0.006f);
        EXPECT_FLOAT_EQ(p[3],1.0f);
    }
    EXPECT_NEAR(mean[0]/count,0.0625f,0.004f);
    EXPECT_NEAR(mean[1]/count,-0.015625f,0.001f);
    EXPECT_NEAR(mean[2]/count,0.03125f,0.002f);
}

TEST_F(CompositionGeometryRendererTest, MinificationMasksRemovedRgbBeforeFilteringAndSupportsAnisotropy) {
    Pattern source(256,64,[](int x,int y) {
        if (x<64) return Pixel{100.0f,50.0f,20.0f,1.0f};
        return Pixel{(x+y)%2 ? 2.0f : 0.0f,-0.2f,0.4f,1.0f};
    }); ASSERT_TRUE(source.frame);
    Clip clip; clip.crop.left = 0.25; clip.transform.height = 2.0;
    auto image = render(source,clip,32,16); ASSERT_TRUE(image) << error;
    // Crop boundary is x=8. No old left-side HDR may contaminate the retained side.
    for (int y=3;y<13;++y) for (int x=11;x<29;++x) {
        const auto p = pixel(*image,x,y);
        EXPECT_NEAR(p[0],1.0f,0.025f);
        EXPECT_NEAR(p[1],-0.2f,0.003f);
        EXPECT_NEAR(p[2],0.4f,0.004f);
    }
    for (int y=3;y<13;++y) for (int x=0;x<5;++x) {
        const auto p = pixel(*image,x,y);
        EXPECT_NEAR(p[0],0.0f,0.005f);
        EXPECT_FLOAT_EQ(p[3],1.0f);
    }
}

TEST_F(CompositionGeometryRendererTest, RepeatedUniformEditsDoNotReuseStaleGeometryOrMutateLeases) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    auto plan = makePlan(96,80);
    auto saved = renderer->render(plan,0,source.frame,&error); ASSERT_TRUE(saved) << error;
    auto before = renderer->download(saved,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(before);
    for (int i=0;i<30;++i) {
        Clip clip; clip.transform.rotation = i*17-220;
        clip.transform.centerX = 0.2+(i%5)*0.13;
        clip.transform.width = 0.6+(i%4)*0.2; clip.transform.height = 0.75+(i%3)*0.15;
        clip.transform.flipHorizontal = i%2; clip.transform.flipVertical = i%3 == 0;
        clip.crop.left = (i%3)*0.125; clip.edgeRounding = (i%5)*0.2;
        clip.edgeSoftness = (i%4)*0.1; clip.opacity = 0.3+(i%5)*0.15;
        checkReference(source,clip,96,80,0.005f);
    }
    auto after = renderer->download(saved,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(after);
    for (int y=0;y<80;++y) for (int x=0;x<96;++x)
        EXPECT_EQ(pixel(*before,x,y),pixel(*after,x,y));
    checkReference(source,{},96,80);
}

TEST_F(CompositionGeometryRendererTest, SoftnessFeathersInwardAndNeverReleasesCroppedPixels) {
    Pattern source(48,32,[](int,int) { return Pixel{1,1,1,1}; });
    Clip clip; clip.crop.left = 0.25;
    auto hard = render(source,clip,48,32); ASSERT_TRUE(hard);
    clip.edgeSoftness = 0.5;
    auto soft = render(source,clip,48,32); ASSERT_TRUE(soft);
    EXPECT_FLOAT_EQ(pixel(*hard,11,16)[0],0.0f);
    EXPECT_FLOAT_EQ(pixel(*soft,11,16)[0],0.0f);
    EXPECT_LT(pixel(*soft,12,16)[0],pixel(*hard,12,16)[0]);
    EXPECT_LT(pixel(*soft,12,16)[0],pixel(*soft,14,16)[0]);
    EXPECT_LT(pixel(*soft,14,16)[0],pixel(*soft,18,16)[0]);
    EXPECT_NEAR(pixel(*soft,24,16)[0],1.0f,0.001f);
    checkReference(source,clip,48,32);
}

TEST_F(CompositionGeometryRendererTest, ExtremeFiniteTransformsCullOrDiagnoseWithoutFillingCanvas) {
    Pattern source(48,32,[](int,int) { return Pixel{1,1,1,1}; });
    Clip clip; clip.transform.width = 1e-300; clip.transform.centerX = 1e100;
    auto offscreen = render(source,clip,48,32); ASSERT_TRUE(offscreen) << error;
    for (int y=0;y<32;++y) for (int x=0;x<48;++x) EXPECT_FLOAT_EQ(pixel(*offscreen,x,y)[0],0.0f);
    clip.transform.centerX = 0.5;
    auto plan = makePlan(48,32,clip);
    EXPECT_FALSE(renderer->render(plan,0,source.frame,&error));
    EXPECT_NE(error.find("coordinate range"),std::string::npos);
}

TEST_F(CompositionGeometryRendererTest, StrictRenderingRejectsUnsupportedAppearanceTracksAndThreeDimensionalRotation) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    std::vector<Clip> cases(8);
    cases[0].transform.rotationX = 1;
    cases[1].transform.rotationY = 1;
    cases[2].positionTrack = KeyframeTrackPayload{};
    cases[3].scaleTrack = KeyframeTrackPayload{};
    cases[4].rotationTrack = KeyframeTrackPayload{};
    cases[5].cropTrack = KeyframeTrackPayload{};
    cases[6].opacityTrack = KeyframeTrackPayload{};
    cases[7].blendMode = "unknown";
    for (const auto& clip : cases) {
        auto plan = makePlan(48,32,clip);
        CompositionRenderRequest request; request.plan = &plan; request.source = source.frame;
        request.clipId = "geometry"; request.strict = true;
        EXPECT_FALSE(renderer->render(request,&error)); EXPECT_NE(error.find("Unsupported"),std::string::npos);
        request.strict = false;
        if (clip.blendMode) {
            EXPECT_FALSE(renderer->render(request,&error));
            EXPECT_NE(error.find("blend"),std::string::npos);
        } else {
            EXPECT_TRUE(renderer->render(request,&error)) << error;
        }
    }
}

TEST_F(CompositionGeometryRendererTest, InvalidConstantGeometryFailsBothStrictAndPreviewRendering) {
    Pattern source(48,32,asymmetric); ASSERT_TRUE(source.frame);
    std::vector<Clip> cases(8);
    cases[0].transform.width = 0;
    cases[1].transform.height = -1;
    cases[2].transform.rotation = std::numeric_limits<double>::quiet_NaN();
    cases[3].crop.left = 0.75; cases[3].crop.right = 0.25;
    cases[4].crop.top = -0.1;
    cases[5].edgeRounding = 1.1;
    cases[6].edgeSoftness = std::numeric_limits<double>::infinity();
    cases[7].transform.centerY = std::numeric_limits<double>::infinity();
    for (const auto& clip : cases) for (bool strict : {false,true}) {
        auto plan = makePlan(96,80,clip);
        CompositionRenderRequest request; request.plan = &plan; request.source = source.frame;
        request.clipId = "geometry"; request.strict = strict;
        EXPECT_FALSE(renderer->render(request,&error)); EXPECT_FALSE(error.empty());
    }
}
} // namespace
