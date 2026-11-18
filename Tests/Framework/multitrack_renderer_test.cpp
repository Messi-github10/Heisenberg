#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
using Pixel = std::array<float,4>;
namespace color = heisenberg::renderer::color;

std::shared_ptr<VideoFrame> solid(Pixel pixel, int width = 32, int height = 24) {
    auto* raw = av_frame_alloc();
    if (!raw) return {};
    auto owned = std::shared_ptr<AVFrame>(raw,[](AVFrame* p) { av_frame_free(&p); });
    raw->width = width; raw->height = height; raw->format = AV_PIX_FMT_RGBAF32;
    raw->sample_aspect_ratio = {1,1}; raw->time_base = {1,24};
    raw->colorspace = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    raw->color_trc = AVCOL_TRC_LINEAR;
    if (av_frame_get_buffer(raw,32) < 0) return {};
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<float*>(raw->data[0]+y*raw->linesize[0]);
        for (int x = 0; x < width; ++x) std::copy(pixel.begin(),pixel.end(),row+x*4);
    }
    return VideoFrame::wrap(std::move(owned),0,Rational{24,1});
}
CompositionPlan planWithTracks(int count = 2) {
    CompositionPlan plan; plan.settings = TimelineSettings::fromTimelineSettings(32,24,24);
    plan.frameCount = 40;
    for (int i = 0; i < count; ++i) {
        CompositionClip clip; clip.id = "clip-"+std::to_string(i);
        clip.mediaRef = clip.id; clip.durationFrames = 40;
        clip.visualSnapshot.id = clip.id; clip.visualSnapshot.durationFrames = 40;
        plan.tracks.push_back({"track-"+std::to_string(i),ClipType::Video,false,false,{clip}});
        plan.colorSettings.inputColorSpaces[clip.mediaRef] = color::kWorkingSpace;
    }
    return plan;
}
CompositionVideoLayer layer(const CompositionPlan& plan, size_t track,
                            std::shared_ptr<VideoFrame> frame, double opacity = 1, int64_t local = 0) {
    return {std::move(frame),plan.tracks[track].id,plan.tracks[track].clips.front().id,local,opacity};
}
Pixel at(const std::shared_ptr<AVFrame>& frame, int x = 16, int y = 12) {
    const auto* pixel = reinterpret_cast<const float*>(frame->data[0]+y*frame->linesize[0])+x*4;
    return {pixel[0],pixel[1],pixel[2],pixel[3]};
}
void expectPixel(Pixel actual, Pixel expected, float tolerance = 0.004f) {
    for (size_t c = 0; c < 4; ++c) EXPECT_NEAR(actual[c],expected[c],tolerance) << "channel " << c;
}
JsonValue exposure(double value, bool animated = false) {
    auto effect = JsonValue::parse(R"({"type":"video.colorGrade","schemaVersion":1,"params":{"exposure":{"value":0}}})");
    effect["params"]["exposure"]["value"] = value;
    if (animated) effect["params"]["exposure"]["track"] = JsonValue::parse(
        R"({"keyframes":[{"frame":0,"value":0,"interpolationOut":"linear"},{"frame":10,"value":1}]})");
    return effect;
}
class MultitrackRendererTest : public ::testing::Test {
protected:
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<CompositionRenderer> renderer;
    uint64_t validationBaseline = 0;
    bool initialized = false;
    void SetUp() override {
        auto& vk = VulkanContext::instance();
        try {
            vk.initLoader(); vk.createInstance(true); vk.createDevice();
            initialized = true; validationBaseline = vk.validationErrorCount();
            VulkanResources resources;
            resources.instance = vk.vkInstance(); resources.physDevice = vk.physicalDevice();
            resources.device = vk.device(); resources.graphicsQF = vk.graphicsQueueFamily();
            resources.graphicsQueue = vk.graphicsQueue(); resources.getProcAddr = vk.getInstanceProcAddr();
            resources.enabledFeatures = vk.enabledDeviceFeatures(); resources.enabledExtensions = vk.enabledDeviceExtensions();
            gpu = std::make_unique<GpuContext>(resources);
        } catch (const std::exception& e) { GTEST_SKIP() << "No windowless Vulkan GPU: " << e.what(); }
        renderer = std::make_unique<CompositionRenderer>(); std::string error;
        ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    }
    void TearDown() override {
        renderer.reset(); gpu.reset();
        if (initialized) EXPECT_EQ(VulkanContext::instance().validationErrorCount(),validationBaseline);
    }
    std::shared_ptr<AVFrame> render(const CompositionPlan& plan, std::vector<CompositionVideoLayer> layers, int64_t frame = 0) {
        std::string error; auto result = renderer->render(plan,frame,layers,&error);
        EXPECT_TRUE(result) << error;
        if (!result) return {};
        EXPECT_EQ(result.image().contract,filtergraph::kWorkingImageContract);
        auto pixels = renderer->download(result,DownloadFormat::RgbaFloat,&error);
        EXPECT_TRUE(pixels) << error; return pixels;
    }
};

TEST_F(MultitrackRendererTest, TopFirstOrderingHalfOpacityAndOpaqueBlack) {
    auto plan = planWithTracks(3);
    auto red = solid({1,0,0,1}), green = solid({0,1,0,1}), blue = solid({0,0,1,1});
    ASSERT_TRUE(red && green && blue);
    auto pixels = render(plan,{layer(plan,0,red,0.5),layer(plan,1,green,0.5),layer(plan,2,blue)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{0.5f,0.25f,0.25f,1});
    // Plan order is authoritative, including callers with shuffled packets.
    pixels = render(plan,{layer(plan,2,blue),layer(plan,0,red,0.5),layer(plan,1,green,0.5)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{0.5f,0.25f,0.25f,1});
    std::swap(plan.tracks[0],plan.tracks[2]);
    pixels = render(plan,{layer(plan,0,blue),layer(plan,1,green,0.5),layer(plan,2,red,0.5)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{0,0,1,1});
    pixels = render(plan,{}); ASSERT_TRUE(pixels); expectPixel(at(pixels),{0,0,0,1});
}

TEST_F(MultitrackRendererTest, SourceAlphaMultipliesOpacityWithoutClippingNegativeHdr) {
    auto plan = planWithTracks();
    auto top = solid({-0.5f,2.0f,0.25f,0.5f}), bottom = solid({0.5f,0.25f,1,1});
    ASSERT_TRUE(top && bottom);
    auto pixels = render(plan,{layer(plan,0,top,0.5),layer(plan,1,bottom)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{0.25f,0.6875f,0.8125f,1});
    pixels = render(plan,{layer(plan,0,top,2),layer(plan,1,bottom,-1)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{-0.25f,1,0.125f,1});
    auto transparent = solid({8,-4,3,0}); ASSERT_TRUE(transparent);
    pixels = render(plan,{layer(plan,0,transparent),layer(plan,1,bottom)});
    ASSERT_TRUE(pixels); expectPixel(at(pixels),{0.5f,0.25f,1,1});
}

TEST_F(MultitrackRendererTest, FitBordersRemainTransparentAndHiddenTrackIsExcluded) {
    auto plan = planWithTracks();
    auto red = solid({1,0,0,1},32,8), blue = solid({0,0,1,1}); ASSERT_TRUE(red && blue);
    // A non-neutral grade must not paint the fitted transparent borders.
    plan.tracks[0].clips[0].visualSnapshot.effects = JsonValue::array({exposure(1)});
    auto pixels = render(plan,{layer(plan,0,red),layer(plan,1,blue)}); ASSERT_TRUE(pixels);
    expectPixel(at(pixels),{2,0,0,1});
    for (const auto position : {std::array<int,2>{0,0},{31,0},{0,23},{31,23},{16,2}})
        expectPixel(at(pixels,position[0],position[1]),{0,0,1,1});
    plan.tracks[0].hidden = true;
    pixels = render(plan,{layer(plan,0,red),layer(plan,1,blue)}); ASSERT_TRUE(pixels);
    expectPixel(at(pixels),{0,0,1,1});
    plan.tracks[1].muted = true; // Audio mute never hides video.
    pixels = render(plan,{layer(plan,1,blue)}); ASSERT_TRUE(pixels); expectPixel(at(pixels),{0,0,1,1});
}

TEST_F(MultitrackRendererTest, EachClipGradesAtItsLocalFrameAndGlobalGraphRunsAfterBlend) {
    auto plan = planWithTracks();
    plan.tracks[0].clips[0].visualSnapshot.effects = JsonValue::array({exposure(0,true)});
    plan.tracks[1].clips[0].visualSnapshot.effects = JsonValue::array({exposure(-1)});
    auto red = solid({0.5f,0,0,0.5f}), blue = solid({0,0,0.8f,1}); ASSERT_TRUE(red && blue);
    for (int64_t local : {0,10,5,0,10}) {
        auto pixels = render(plan,{layer(plan,0,red,1,local),layer(plan,1,blue,1,3)},12);
        ASSERT_TRUE(pixels);
        expectPixel(at(pixels),{0.25f*std::exp2(float(local)/10),0,0.2f,1});
    }
    // A nonlinear global contrast DAG distinguishes grading the composite
    // once from independently filtering each source before the blend.
    Clip global;
    global.effects = JsonValue::parse(R"([{"type":"video.colorGrade","schemaVersion":1,"params":{"contrast":{"value":1.2}}}])");
    grade::CompiledGrade compiled; color::ColorPipeline pipeline; std::string error;
    ASSERT_TRUE(grade::GradeCompiler::compile(global,0,compiled,&error,{},&pipeline)) << error;
    ASSERT_TRUE(renderer->setFilterGraphSnapshot(compiled.document,&error)) << error;
    auto actual = render(plan,{layer(plan,0,red,1,10),layer(plan,1,blue)}); ASSERT_TRUE(actual);
    ASSERT_TRUE(renderer->setFilterGraphSnapshot({},&error)) << error;
    auto referencePlan = planWithTracks(1); referencePlan.tracks[0].clips[0].visualSnapshot = global;
    auto composite = solid({0.5f,0,0.2f,1}); ASSERT_TRUE(composite);
    auto expected = render(referencePlan,{layer(referencePlan,0,composite)}); ASSERT_TRUE(expected);
    expectPixel(at(actual),at(expected),0.008f);
}

TEST_F(MultitrackRendererTest, FinishedRec709ShortcutRequiresAllVisibleContributions) {
    auto plan = planWithTracks();
    for (const auto& track : plan.tracks) plan.colorSettings.inputColorSpaces[track.clips[0].mediaRef] = color::kExportDisplay;
    auto gray = solid({0.4f,0.4f,0.4f,1}); ASSERT_TRUE(gray); std::string error;
    const ColorOutputRequest output{color::kPreviewDisplay,color::kColorimetricView,{},ColorOutputIntent::SdrVideoPreview};
    auto check = [&](const std::string& expected) {
        auto composite = renderer->render(plan,0,std::vector<CompositionVideoLayer>{layer(plan,0,gray,0.5),layer(plan,1,gray)},&error);
        EXPECT_TRUE(composite) << error; if (!composite) return;
        auto encoded = renderer->output(composite,output,&error); EXPECT_TRUE(encoded) << error;
        if (encoded) EXPECT_EQ(encoded.outputSelectionMetadata().display,expected);
    };
    check(color::kExportDisplay);
    plan.colorSettings.inputColorSpaces[plan.tracks[1].clips[0].mediaRef] = color::kWorkingSpace;
    check(color::kPreviewDisplay);
    plan.tracks[1].hidden = true;
    check(color::kExportDisplay);
}
} // namespace
