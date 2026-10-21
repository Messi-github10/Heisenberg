#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/LutLibrary.hpp>
#include <Models/ProjectFile.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::shared_ptr<VideoFrame> fixedSource(int width, int height) {
    auto* raw = av_frame_alloc();
    if (!raw) return {};
    auto frame = std::shared_ptr<AVFrame>(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->width = width; raw->height = height; raw->format = AV_PIX_FMT_RGBAF32;
    raw->sample_aspect_ratio = {1, 1}; raw->color_range = AVCOL_RANGE_JPEG;
    raw->colorspace = AVCOL_SPC_RGB; raw->color_trc = AVCOL_TRC_LINEAR;
    raw->pts = 0; raw->time_base = {1, 24};
    if (av_frame_get_buffer(raw, 32) < 0) return {};
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<float*>(raw->data[0] + y * raw->linesize[0]);
        for (int x = 0; x < width; ++x) {
            row[x * 4] = 0.02f + 0.7f * x / width;
            row[x * 4 + 1] = 0.03f + 0.6f * y / height;
            row[x * 4 + 2] = 0.04f + 0.4f * (x + y) / (width + height);
            row[x * 4 + 3] = 1;
        }
    }
    return VideoFrame::wrap(std::move(frame), 0, Rational{24, 1});
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

class ColorGradePerformanceTest : public testing::TestWithParam<std::array<int, 2>> {};
} // namespace

TEST_P(ColorGradePerformanceTest, FullPipelineColdAndFiveWarmFramesReportApiWallTime) {
    const auto size = GetParam();
    auto& vk = VulkanContext::instance();
    try {
        vk.initLoader(); vk.createInstance(false); vk.createDevice();
    } catch (const std::exception& e) { GTEST_SKIP() << "No Vulkan GPU: " << e.what(); }
    VulkanResources resources{vk.vkInstance(), vk.physicalDevice(), vk.device(),
        vk.graphicsQueueFamily(), vk.graphicsQueue(), vk.getInstanceProcAddr()};
    resources.enabledFeatures = vk.enabledDeviceFeatures(); resources.enabledExtensions = vk.enabledDeviceExtensions();
    GpuContext gpu(resources);
    const auto validationBaseline = vk.validationErrorCount();
    auto source = fixedSource(size[0], size[1]);
    ASSERT_TRUE(source);
    struct TemporaryLibrary {
        fs::path root = fs::temp_directory_path() / ("heisenberg-grade-performance-" + makeUuid());
        ~TemporaryLibrary() { std::error_code ignored; fs::remove_all(root, ignored); }
    } library;
    fs::create_directories(library.root);
    const auto cube = library.root / "creative.cube";
    {
        std::ofstream out(cube);
        out << "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b) for (int g = 0; g < 2; ++g) for (int r = 0; r < 2; ++r)
            out << 0.9 * r << ' ' << 0.95 * g << ' ' << 0.85 * b << '\n';
    }
    std::string error;
    const auto asset = grade::LutLibrary(library.root / "assets").importFile(cube, &error);
    ASSERT_TRUE(asset) << error;
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(size[0], size[1], 24); plan.frameCount = 6;
    plan.colorSettings.inputColorSpaces["synthetic"] = color::kWorkingSpace;
    plan.gradeLuts.emplace(asset->id, asset);
    CompositionClip clip; clip.id = "graded"; clip.mediaRef = "synthetic"; clip.durationFrames = 6;
    clip.visualSnapshot.id = clip.id; clip.visualSnapshot.mediaRef = clip.mediaRef; clip.visualSnapshot.durationFrames = 6;
    // All video.colorGrade v1 controls are active: linear exposure/WB, ACEScct
    // tone, encoded-space presence/wheels/curves/hue targets, then creative LUT.
    auto effect = JsonValue::parse(R"({"type":"video.colorGrade","schemaVersion":1,"params":{
        "exposure":{"value":0.25},"temperature":{"value":7000},"tint":{"value":5},"contrast":{"value":1.1},
        "highlights":{"value":-0.1},"shadows":{"value":0.1},"blacks":{"value":0.02},"whites":{"value":-0.02},
        "vibrance":{"value":0.15},"saturation":{"value":1.1},
        "liftHue":{"value":30},"liftStrength":{"value":0.05},"liftMaster":{"value":0.01},
        "gammaHue":{"value":240},"gammaStrength":{"value":0.05},"gammaMaster":{"value":1.05},
        "gainHue":{"value":120},"gainStrength":{"value":0.05},"gainMaster":{"value":1.02},
        "curves":{"master":[[0,0],[0.4,0.45],[1,1]],"red":[[0,0],[0.5,0.52],[1,1]],
                  "green":[[0,0],[0.5,0.48],[1,1]],"blue":[[0,0],[0.5,0.54],[1,1]]},
        "hueTargets":[{"hue":355,"rotation":10,"saturation":1.1,"lightness":0.02},
                      {"hue":120,"rotation":-5,"saturation":0.9,"lightness":-0.01}]
    }})");
    effect["params"]["lut"] = {{"id", asset->id}, {"domain", "rec709"}, {"intensity", {{"value", 0.6}}}};
    clip.visualSnapshot.effects = JsonValue::array({effect}); plan.clips.push_back(clip);
    std::vector<double> cold, warm;
    for (int trial = 0; trial < 3; ++trial) {
        CompositionRenderer renderer;
        ASSERT_TRUE(renderer.initialize(gpu.plGpu(), gpu.plVulkan(), nullptr, nullptr, &error)) << error;
        const auto measure = [&](int64_t frame, std::vector<double>& samples) {
            const auto begin = Clock::now();
            auto result = renderer.render(plan, frame, source, &error);
            ASSERT_TRUE(result) << error;
            // Include completion and lease destruction. Never download/readback:
            // this is renderer API wall time, not a CPU transfer benchmark.
            pl_gpu_finish(gpu.plGpu());
            ASSERT_EQ(synchronizedQueueWaitIdle(vk.graphicsQueue()), VK_SUCCESS);
            result = {};
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        };
        ASSERT_NO_FATAL_FAILURE(measure(0, cold));
        if (trial == 2) for (int frame = 1; frame <= 5; ++frame)
            ASSERT_NO_FATAL_FAILURE(measure(frame, warm));
        renderer.shutdown();
    }
    const double coldMedian = median(cold), warmMedian = median(warm);
    EXPECT_TRUE(std::isfinite(coldMedian)); EXPECT_TRUE(std::isfinite(warmMedian));
    RecordProperty("width", size[0]); RecordProperty("height", size[1]);
    RecordProperty("cold_renderer_samples", 3); RecordProperty("warm_frames", 5);
    RecordProperty("cold_api_wall_median_ms", std::to_string(coldMedian));
    RecordProperty("warm_api_wall_median_ms", std::to_string(warmMedian));
    std::cout << "ColorGrade " << size[0] << 'x' << size[1]
              << " renderer+GPU-wait API wall: cold median=" << coldMedian
              << " ms (3 fresh renderers), warm median=" << warmMedian << " ms (5 frames)\n";
    EXPECT_EQ(vk.validationErrorCount(), validationBaseline);
}

INSTANTIATE_TEST_SUITE_P(Resolution, ColorGradePerformanceTest,
    testing::Values(std::array<int, 2>{1920, 1080}, std::array<int, 2>{3840, 2160}),
    [](const testing::TestParamInfo<std::array<int, 2>>& info) {
        return std::to_string(info.param[0]) + "x" + std::to_string(info.param[1]);
    });
