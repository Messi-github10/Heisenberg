#include <Compositing/CompositionPlan.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
namespace color = heisenberg::renderer::color;

struct WindowlessGpu {
    std::unique_ptr<GpuContext> imported;
    pl_vulkan vk = nullptr;
    std::string error;
    WindowlessGpu() {
        try {
            auto& context = VulkanContext::instance();
            context.initLoader();
            if (!context.vkInstance()) context.createInstance(false);
            if (!context.device()) context.createDevice();
            VulkanResources resources;
            resources.instance = context.vkInstance();
            resources.physDevice = context.physicalDevice();
            resources.device = context.device();
            resources.graphicsQF = context.graphicsQueueFamily();
            resources.graphicsQueue = context.graphicsQueue();
            resources.getProcAddr = context.getInstanceProcAddr();
            resources.enabledFeatures = context.enabledDeviceFeatures();
            resources.enabledExtensions = context.enabledDeviceExtensions();
            imported = std::make_unique<GpuContext>(resources);
            vk = imported->plVulkan();
        } catch (const std::exception& exception) { error = exception.what(); }
    }
};

std::shared_ptr<VideoFrame> sourceFrame(std::array<float,4> value, bool impulse = false) {
    auto* raw = av_frame_alloc();
    if (!raw) return {};
    auto frame = std::shared_ptr<AVFrame>(raw, [](AVFrame* value) { av_frame_free(&value); });
    raw->width = 17;
    raw->height = 19;
    raw->format = AV_PIX_FMT_RGBAF32;
    raw->sample_aspect_ratio = {1,1};
    raw->color_range = AVCOL_RANGE_JPEG;
    raw->colorspace = AVCOL_SPC_RGB;
    raw->color_trc = AVCOL_TRC_LINEAR;
    raw->pts = 0;
    raw->time_base = {1,24};
    if (av_frame_get_buffer(raw,32) < 0) return {};
    for (int y=0; y<raw->height; ++y) {
        auto* row = reinterpret_cast<float*>(raw->data[0]+y*raw->linesize[0]);
        for (int x=0; x<raw->width; ++x) {
            const bool lit = !impulse || (x == 8 && y == 9);
            for (int c=0; c<4; ++c) row[x*4+c] = c==3 || lit ? value[c] : 0.0f;
        }
    }
    return VideoFrame::wrap(std::move(frame),0,Rational{24,1});
}

CompositionPlan planWith(const char* effects = "[]") {
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(17,19,24);
    plan.frameCount = 24;
    CompositionClip clip;
    clip.id = "graded";
    clip.mediaRef = "media";
    clip.durationFrames = 24;
    clip.visualSnapshot.id = clip.id;
    clip.visualSnapshot.mediaRef = clip.mediaRef;
    clip.visualSnapshot.durationFrames = 24;
    clip.visualSnapshot.effects = JsonValue::parse(effects);
    plan.clips.push_back(clip);
    plan.colorSettings.inputColorSpaces["media"] = color::kWorkingSpace;
    return plan;
}

std::array<float,4> pixelAt(const std::shared_ptr<AVFrame>& frame, int x=8, int y=9) {
    if (!frame) return {};
    const auto* data = reinterpret_cast<const float*>(frame->data[0]+y*frame->linesize[0])+x*4;
    return {data[0],data[1],data[2],data[3]};
}

std::shared_ptr<AVFrame> renderDownload(CompositionRenderer& renderer,
                                      const CompositionPlan& plan,
                                      const std::shared_ptr<VideoFrame>& source,
                                      int64_t frame, std::string& error) {
    auto result = renderer.render(plan,frame,source,&error);
    if (!result) return {};
    return renderer.download(result,DownloadFormat::RgbaFloat,&error);
}

} // namespace

TEST(GradeRendererTest, RealExposureGraphAnimationCachedParametersAndNeutralFastPath) {
    WindowlessGpu gpu;
    if (!gpu.vk) GTEST_SKIP() << "No Vulkan GPU for windowless Grade integration: " << gpu.error;
    CompositionRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.initialize(gpu.vk->gpu,gpu.vk,nullptr,nullptr,&error)) << error;
    auto source = sourceFrame({0.18f,0.4f,-0.05f,1.0f});
    ASSERT_TRUE(source);
    auto neutral = planWith();
    auto baselineFrame = renderDownload(renderer,neutral,source,0,error);
    ASSERT_TRUE(baselineFrame) << error;
    const auto baseline = pixelAt(baselineFrame);
    auto plan = planWith(R"([{"type":"video.exposure","params":{"exposure":{"track":{"keyframes":[{"frame":0,"value":0,"interpolationOut":"linear"},{"frame":10,"value":2}]}}}}])");
    for (const int frame : {0,5,10,5,0}) {
        auto output = renderDownload(renderer,plan,source,frame,error);
        ASSERT_TRUE(output) << error;
        const auto pixel = pixelAt(output);
        const float gain = std::exp2(static_cast<float>(frame)/5.0f);
        for (int c=0; c<3; ++c) EXPECT_NEAR(pixel[c],baseline[c]*gain,0.002f) << frame << " channel " << c;
        EXPECT_NEAR(pixel[3],baseline[3],0.001f);
    }
    EXPECT_LT(baseline[2],0.0f); // IDT/Grade must preserve legitimate negatives.
}

TEST(GradeRendererTest, ACEScctStagesMatchColorProcessorReferenceAndPreserveAlpha) {
    WindowlessGpu gpu;
    if (!gpu.vk) GTEST_SKIP() << "No Vulkan GPU for windowless Grade integration: " << gpu.error;
    CompositionRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.initialize(gpu.vk->gpu,gpu.vk,nullptr,nullptr,&error)) << error;
    auto source = sourceFrame({0.18f,0.4f,0.05f,1.0f});
    auto neutral = planWith();
    auto baseline = renderDownload(renderer,neutral,source,0,error);
    ASSERT_TRUE(baseline) << error;
    auto reference = pixelAt(baseline);
    color::ColorPipeline pipeline;
    pipeline.processor(color::kWorkingSpace,color::kGradingSpace).applyRGBA(reference.data(),1);
    const float pivot = 0.4135884024924423f;
    for (int c=0; c<3; ++c) reference[c] = (reference[c]-pivot)*1.4f+pivot;
    const float achromatic = reference[0]*0.2722287168f+reference[1]*0.6740817658f+reference[2]*0.0536895174f;
    for (int c=0; c<3; ++c) reference[c] = achromatic+(reference[c]-achromatic)*0.6f;
    pipeline.processor(color::kGradingSpace,color::kWorkingSpace).applyRGBA(reference.data(),1);
    auto plan = planWith(R"([{"type":"video.basicGrade","params":{"contrast":{"value":1.4},"saturation":{"value":0.6}}}])");
    auto output = renderDownload(renderer,plan,source,0,error);
    ASSERT_TRUE(output) << error;
    const auto actual = pixelAt(output);
    for (int c=0; c<3; ++c) EXPECT_NEAR(actual[c],reference[c],0.004f) << c;
    EXPECT_NEAR(actual[3],reference[3],0.001f);
}

TEST(GradeRendererTest, TwoPassGaussianSpreadsBothAxesAndPreservesLinearConstant) {
    WindowlessGpu gpu;
    if (!gpu.vk) GTEST_SKIP() << "No Vulkan GPU for windowless Grade integration: " << gpu.error;
    CompositionRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.initialize(gpu.vk->gpu,gpu.vk,nullptr,nullptr,&error)) << error;
    auto plan = planWith(R"([{"type":"video.blur","params":{"radius":{"value":2},"sigma":{"value":1}}}])");
    auto impulse = sourceFrame({1.0f,0.5f,0.25f,1.0f},true);
    auto output = renderDownload(renderer,plan,impulse,0,error);
    ASSERT_TRUE(output) << error;
    const float center = pixelAt(output)[0];
    EXPECT_GT(center,0.0f);
    EXPECT_LT(center,0.5f);
    EXPECT_GT(pixelAt(output,9,9)[0],0.02f);
    EXPECT_GT(pixelAt(output,8,10)[0],0.02f);
    EXPECT_NEAR(pixelAt(output,9,9)[0],pixelAt(output,8,10)[0],0.005f);
    EXPECT_NEAR(pixelAt(output,0,0)[0],0.0f,0.001f);
    auto constant = sourceFrame({0.5f,-0.1f,1.5f,1.0f});
    auto baseline = renderDownload(renderer,planWith(),constant,0,error);
    ASSERT_TRUE(baseline) << error;
    auto blurred = renderDownload(renderer,plan,constant,0,error);
    ASSERT_TRUE(blurred) << error;
    const auto expected = pixelAt(baseline);
    for (const std::pair<int,int> coordinate : {std::pair{0,0},std::pair{8,9},std::pair{16,18}}) {
        const auto pixel = pixelAt(blurred,coordinate.first,coordinate.second);
        for (int c=0; c<4; ++c) EXPECT_NEAR(pixel[c],expected[c],0.003f);
    }
}

TEST(GradeRendererTest, StrictUnknownEffectFailsRatherThanRenderingUnprocessedImage) {
    WindowlessGpu gpu;
    if (!gpu.vk) GTEST_SKIP() << "No Vulkan GPU for windowless Grade integration: " << gpu.error;
    CompositionRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.initialize(gpu.vk->gpu,gpu.vk,nullptr,nullptr,&error)) << error;
    auto source = sourceFrame({0.18f,0.4f,0.05f,1.0f});
    auto plan = planWith(R"([{"id":"unsupported","type":"future.grade","enabled":true}])");
    CompositionRenderRequest request;
    request.plan = &plan;
    request.source = source;
    request.clipId = "graded";
    request.strict = true;
    auto result = renderer.render(request,&error);
    EXPECT_FALSE(result);
    EXPECT_NE(error.find("future.grade"),std::string::npos);
    request.strict = false;
    std::vector<std::string> diagnostics;
    request.diagnostics = &diagnostics;
    auto preview = renderer.render(request,&error);
    EXPECT_TRUE(preview) << error;
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const std::string& diagnostic) {
        return diagnostic.find("future.grade") != std::string::npos;
    }));
}
