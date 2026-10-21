#include <Compositing/CompositionBuilder.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <Compositing/Grade/ColorGrade.hpp>
#include <Compositing/Grade/GradeGpuPrograms.hpp>
#include <Compositing/Grade/LutLibrary.hpp>
#include <Export/ExportEngine.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {
using namespace heisenberg;
using namespace heisenberg::grade;
using namespace heisenberg::renderer;
namespace fs = std::filesystem;
using Pixel = std::array<float, 4>;
using Patches = std::array<double, 8>;
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

JsonValue fullGrade() {
    // video.colorGrade schemaVersion 1: all three processing domains, wheels,
    // exact piecewise curves, cyclic hue targets, and clip-local animation.
    return JsonValue::parse(R"({"id":"grade","type":"video.colorGrade","schemaVersion":1,"enabled":true,"params":{
        "exposure":{"value":0.2,"track":{"keyframes":[{"frame":0,"value":0.2,"interpolationOut":"linear"},{"frame":5,"value":0.7}]}},
        "contrast":{"value":1.1},"highlights":{"value":-0.1},"shadows":{"value":0.08},"blacks":{"value":0.03},"whites":{"value":-0.04},
        "temperature":{"value":7100},"tint":{"value":4},"vibrance":{"value":0.12},"saturation":{"value":0.85},
        "liftHue":{"value":25},"liftStrength":{"value":0.03},"liftMaster":{"value":0.01},
        "gammaHue":{"value":240},"gammaStrength":{"value":0.02},"gammaMaster":{"value":1.04},
        "gainHue":{"value":110},"gainStrength":{"value":0.03},"gainMaster":{"value":0.98},
        "curves":{"master":[[0,0],[0.4,0.44],[1,1]],"red":[[0,0],[0.6,0.58],[1,1]]},
        "hueTargets":[{"hue":355,"rotation":5,"saturation":0.95,"lightness":0.01}]
    }})");
}

ProjectSnapshot capture(const JsonValue& effect) {
    VideoProject project;
    ProjectFile file;
    Timeline timeline;
    timeline.id = "timeline";
    timeline.width = 160; timeline.height = 120; timeline.fps = 24;
    Track track; track.id = "video";
    Clip clip; clip.id = "clip"; clip.mediaRef = "gray"; clip.durationFrames = 6;
    clip.effects = JsonValue::array({effect});
    track.clips.push_back(clip); timeline.tracks.push_back(track);
    file.timelines.push_back(timeline); file.activeTimelineId = timeline.id;
    file.colorSettings.inputColorSpaces["gray"] = color::kExportDisplay;
    MediaManifest manifest;
    MediaManifestEntry media;
    media.id = "gray"; media.duration = 0.25;
    media.source = {MediaSource::Kind::External, HEISENBERG_TEST_SDR_GRAY};
    EXPECT_TRUE(manifest.add(media));
    std::string error;
    EXPECT_TRUE(project.editor().replaceDocument(std::move(file), std::move(manifest), {}, &error)) << error;
    return project.snapshot();
}

Patches rgbaPatches(const AVFrame& frame) {
    Patches result{};
    for (int patch = 0; patch < 8; ++patch) {
        for (int y = 16; y < 104; ++y) {
            const auto* row = reinterpret_cast<const float*>(frame.data[0] + y * frame.linesize[0]);
            for (int x = patch * 20 + 5; x < patch * 20 + 15; ++x) {
                const float* p = row + x * 4;
                result[patch] += 16 + 219 * (0.2126 * std::clamp(p[0], 0.0f, 1.0f)
                    + 0.7152 * std::clamp(p[1], 0.0f, 1.0f) + 0.0722 * std::clamp(p[2], 0.0f, 1.0f));
            }
        }
        result[patch] /= 880;
    }
    return result;
}

// Every production graph/output node stores RGBA16F. Model round-to-nearest,
// ties-to-even exactly, including subnormals; ACEScct inverse amplifies even
// small omitted storage roundings. The input download already includes IDT
// and libplacebo source unpacking, which this grade oracle must not duplicate.
Pixel halfRound(Pixel pixel) {
    for (float& value : pixel) {
        if (!std::isfinite(value) || value == 0) continue;
        const float magnitude = std::abs(value);
        if (magnitude >= 65520.0f) {
            value = std::copysign(std::numeric_limits<float>::infinity(), value);
            continue;
        }
        int exponent = 0;
        std::frexp(magnitude, &exponent);
        const float step = std::ldexp(1.0f, std::max(exponent - 11, -24));
        const float scaled = magnitude / step;
        float rounded = std::floor(scaled);
        const float remainder = scaled - rounded;
        if (remainder > 0.5f || (remainder == 0.5f && std::fmod(rounded, 2.0f) != 0)) ++rounded;
        value = std::copysign(rounded * step, value);
    }
    return pixel;
}

std::vector<Patches> decodeLuma(const fs::path& path) {
    AVFormatContext* input = nullptr;
    EXPECT_GE(avformat_open_input(&input, utf8(path).c_str(), nullptr, nullptr), 0);
    if (!input) return {};
    const auto closeInput = [](AVFormatContext* p) { avformat_close_input(&p); };
    std::unique_ptr<AVFormatContext, decltype(closeInput)> format(input, closeInput);
    EXPECT_GE(avformat_find_stream_info(input, nullptr), 0);
    const int index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) { ADD_FAILURE() << "No exported video"; return {}; }
    EXPECT_EQ(input->streams[index]->codecpar->codec_id, AV_CODEC_ID_H264);
    EXPECT_EQ(input->streams[index]->codecpar->color_space, AVCOL_SPC_BT709);
    AVCodecContext* raw = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    const auto closeDecoder = [](AVCodecContext* p) { avcodec_free_context(&p); };
    std::unique_ptr<AVCodecContext, decltype(closeDecoder)> decoder(raw, closeDecoder);
    if (!raw) return {};
    EXPECT_GE(avcodec_parameters_to_context(raw, input->streams[index]->codecpar), 0);
    EXPECT_GE(avcodec_open2(raw, nullptr, nullptr), 0);
    AVFrame* frame = av_frame_alloc(); AVPacket* packet = av_packet_alloc();
    std::vector<Patches> result;
    const auto pull = [&] {
        int status;
        while ((status = avcodec_receive_frame(raw, frame)) == 0) {
            EXPECT_EQ(frame->width, 160); EXPECT_EQ(frame->height, 120);
            if (frame->width == 160 && frame->height == 120 && frame->format == AV_PIX_FMT_YUV420P) {
                Patches values{};
                for (int p = 0; p < 8; ++p) {
                    for (int y = 16; y < 104; ++y)
                        for (int x = p * 20 + 5; x < p * 20 + 15; ++x)
                            values[p] += frame->data[0][y * frame->linesize[0] + x];
                    values[p] /= 880;
                }
                result.push_back(values);
            } else ADD_FAILURE() << "Unexpected H264 decoded format";
            av_frame_unref(frame);
        }
        EXPECT_TRUE(status == AVERROR(EAGAIN) || status == AVERROR_EOF);
    };
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == index) { EXPECT_GE(avcodec_send_packet(raw, packet), 0); pull(); }
        av_packet_unref(packet);
    }
    EXPECT_GE(avcodec_send_packet(raw, nullptr), 0); pull();
    av_packet_free(&packet); av_frame_free(&frame);
    return result;
}

class ColorGradeExportTest : public testing::Test {
protected:
    fs::path root = fs::temp_directory_path() / ("heisenberg-color-grade-export-" + makeUuid());
    fs::path previousRoot = LutLibrary::configuredRoot();
    void SetUp() override { fs::create_directories(root); }
    void TearDown() override {
        LutLibrary::setDefaultRoot(previousRoot);
        std::error_code ignored; fs::remove_all(root, ignored);
    }
    LutLibrary::Snapshot importLut() {
        const auto source = root / "creative.cube";
        {
            std::ofstream stream(source);
            stream << "TITLE \"Creative test\"\nLUT_3D_SIZE 2\n";
            for (int b = 0; b < 2; ++b) for (int g = 0; g < 2; ++g) for (int r = 0; r < 2; ++r)
                stream << 0.8 * r + 0.03 << ' ' << 0.9 * g + 0.02 << ' ' << 0.7 * b + 0.04 << '\n';
        }
        std::string error;
        auto asset = LutLibrary(root / "library").importFile(source, &error);
        EXPECT_TRUE(asset) << error;
        LutLibrary::setDefaultRoot(root / "library");
        return asset;
    }
};
} // namespace

TEST_F(ColorGradeExportTest, H264FullGradeMatchesPreviewAndIndependentCpuReference) {
    const auto effect = fullGrade();
    const auto snapshot = capture(effect);
    CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(CompositionBuilder::build(snapshot, plan, &error)) << error;
    auto& vk = VulkanContext::instance();
    vk.initLoader(); vk.createInstance(false); vk.createDevice();
    VulkanResources resources{vk.vkInstance(), vk.physicalDevice(), vk.device(),
        vk.graphicsQueueFamily(), vk.graphicsQueue(), vk.getInstanceProcAddr()};
    resources.enabledFeatures = vk.enabledDeviceFeatures(); resources.enabledExtensions = vk.enabledDeviceExtensions();
    GpuContext gpu(resources);
    CompositionRenderer renderer;
    ASSERT_TRUE(renderer.initialize(gpu.plGpu(), gpu.plVulkan(), nullptr, nullptr, &error)) << error;
    CompositionExecutor executor;
    ASSERT_TRUE(executor.open(plan, {false, CompositionErrorPolicy::Strict}));
    color::ColorPipeline color;
    std::vector<Patches> expected;
    auto ungraded = plan;
    for (auto& clip : ungraded.clips) clip.visualSnapshot.effects = JsonValue::array();
    CompositionRenderer baselineRenderer;
    ASSERT_TRUE(baselineRenderer.initialize(gpu.plGpu(), gpu.plVulkan(), nullptr, nullptr, &error)) << error;
    for (int frame = 0; frame < 6; ++frame) {
        const auto decoded = executor.videoFrame(frame);
        ASSERT_TRUE(decoded) << decoded.error;
        // Use the same decoded source and normalization path, with only grade
        // disabled. This isolates grade math from YUV unpack/IDT precision.
        auto baseline = baselineRenderer.render(ungraded, frame, decoded.value.source, &error);
        ASSERT_TRUE(baseline) << error;
        auto baselinePixels = baselineRenderer.download(baseline, DownloadFormat::RgbaFloat, &error);
        ASSERT_TRUE(baselinePixels) << error;
        CompositionRenderRequest preview;
        preview.plan = &plan; preview.settings = plan.settings; preview.source = decoded.value.source;
        preview.targetFrame = frame; preview.clipLocalFrame = frame; preview.clipId = "clip"; preview.strict = false;
        auto working = renderer.render(preview, &error);
        ASSERT_TRUE(working) << error;
        ColorOutputRequest output; output.display = plan.colorSettings.exportOutput.display;
        output.view = plan.colorSettings.exportOutput.view;
        auto encoded = renderer.output(working, output, &error);
        ASSERT_TRUE(encoded) << error;
        auto pixels = renderer.download(encoded, DownloadFormat::RgbaFloat, &error);
        ASSERT_TRUE(pixels) << error;
        expected.push_back(rgbaPatches(*pixels));
        ColorGradeState state; std::vector<GradeDiagnostic> diagnostics;
        ASSERT_TRUE(sampleColorGrade(effect, frame, state, diagnostics, &error)) << error;
        const auto toTone = color.processor(color::kWorkingSpace, color::kGradingSpace);
        const auto toAux = color.processor(color::kGradingSpace, kColorGradeAuxiliarySpace);
        const auto toWorking = color.processor(kColorGradeAuxiliarySpace, color::kWorkingSpace);
        const auto toOutput = color.displayProcessor(color::kWorkingSpace, output.display, output.view);
        for (int p = 0; p < 8; ++p) {
            double referenceY = 0;
            // Match the preview patch's exact spatial samples rather than
            // assuming a decoder code value or averaging before nonlinear grade.
            for (int y = 16; y < 104; ++y) {
                const auto* row = reinterpret_cast<const float*>(baselinePixels->data[0]
                    + y * baselinePixels->linesize[0]);
                for (int x = p * 20 + 5; x < p * 20 + 15; ++x) {
                    const float* input = row + x * 4;
                    Pixel reference = halfRound({input[0], input[1], input[2], input[3]});
                    reference = halfRound(referenceColorGradePixel(state, ColorGradeProgramDomain::LinearACEScg, reference));
                    toTone.applyRGBA(reference.data(), 1);
                    reference = halfRound(reference);
                    reference = halfRound(referenceColorGradePixel(state, ColorGradeProgramDomain::ToneACEScct, reference));
                    toAux.applyRGBA(reference.data(), 1);
                    reference = halfRound(reference);
                    reference = halfRound(referenceColorGradePixel(state, ColorGradeProgramDomain::Auxiliary, reference));
                    // This full-grade fixture has no creative LUT. Its only
                    // remaining nodes are auxiliary -> working and display.
                    toWorking.applyRGBA(reference.data(), 1);
                    reference = halfRound(reference);
                    toOutput.applyRGBA(reference.data(), 1);
                    reference = halfRound(reference);
                    referenceY += 16 + 219 * (0.2126 * std::clamp(reference[0], 0.0f, 1.0f)
                        + 0.7152 * std::clamp(reference[1], 0.0f, 1.0f) + 0.0722 * std::clamp(reference[2], 0.0f, 1.0f));
                }
            }
            EXPECT_NEAR(expected.back()[p], referenceY / 880, 1.5) << "CPU reference frame " << frame << " patch " << p;
        }
    }
    baselineRenderer.shutdown();
    renderer.shutdown();
    ExportEngine engine; ExportRequest request;
    request.snapshot = snapshot; request.outputPath = utf8(root / "graded.mp4");
    request.hardwareDecode = false; request.videoCodec = ExportVideoCodec::H264; request.crf = 0;
    ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
    ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
    const auto actual = decodeLuma(root / "graded.mp4");
    ASSERT_EQ(actual.size(), 6u);
    for (size_t f = 0; f < actual.size(); ++f) for (int p = 0; p < 8; ++p)
        EXPECT_NEAR(actual[f][p], expected[f][p], 2.0) << "preview/export frame " << f << " patch " << p;
    EXPECT_GT(std::abs(actual.front()[4] - actual.back()[4]), 3.0); // Animation reached the export.
}

TEST_F(ColorGradeExportTest, MissingLutFailsStrictlyWithoutReplacingExistingOutput) {
    LutLibrary::setDefaultRoot(root / "empty-library");
    auto effect = fullGrade();
    effect["params"]["lut"] = {{"id", std::string(64, 'a')}, {"name", "Missing"}, {"domain", "rec709"}, {"intensity", {{"value", 1}}}};
    const auto path = root / "preserved.mp4";
    { std::ofstream out(path); out << "preserve"; }
    ExportEngine engine; ExportRequest request;
    request.snapshot = capture(effect); request.outputPath = utf8(path); request.overwrite = true;
    ASSERT_TRUE(engine.start(request)); engine.wait();
    EXPECT_EQ(engine.result().state, ExportState::Failed);
    EXPECT_NE(engine.result().error.find("LUT"), std::string::npos);
    std::ifstream stream(path); std::string text; stream >> text;
    EXPECT_EQ(text, "preserve");
    for (const auto& entry : fs::directory_iterator(root)) EXPECT_NE(entry.path().extension(), ".partial");
}

TEST_F(ColorGradeExportTest, StartPinsLutAcrossRootSwitchInvalidationAndResourceRemoval) {
    const auto asset = importLut(); ASSERT_TRUE(asset);
    auto effect = fullGrade();
    effect["params"]["lut"] = {{"id", asset->id}, {"name", asset->displayName}, {"domain", "rec709"}, {"intensity", {{"value", 0.75}}}};
    const auto snapshot = capture(effect);
    ExportRequest request; request.snapshot = snapshot; request.hardwareDecode = false;
    request.crf = 0; request.outputPath = utf8(root / "baseline.mp4");
    ExportEngine baseline; ASSERT_TRUE(baseline.start(request)); baseline.wait();
    ASSERT_TRUE(baseline.result().succeeded()) << baseline.result().error;
    ExportEngine pinned;
    // Also switch on the worker before renderer initialization: validation and
    // frame compilation must both use the task plan, never the configured root.
    pinned.onProgress = [&](const ExportProgress& progress) {
        if (progress.state == ExportState::Preparing) LutLibrary::setDefaultRoot(root / "other-library");
    };
    request.outputPath = utf8(root / "pinned.mp4");
    ASSERT_TRUE(pinned.start(request));
    LutLibrary::invalidateConfigured();
    LutLibrary::setDefaultRoot(root / "other-library");
    ASSERT_TRUE(fs::remove(asset->resourcePath));
    pinned.wait();
    ASSERT_TRUE(pinned.result().succeeded()) << pinned.result().stage << ": " << pinned.result().error;
    const auto expected = decodeLuma(root / "baseline.mp4");
    const auto actual = decodeLuma(root / "pinned.mp4");
    ASSERT_EQ(expected.size(), 6u); ASSERT_EQ(actual.size(), expected.size());
    for (size_t f = 0; f < actual.size(); ++f) for (int p = 0; p < 8; ++p)
        EXPECT_NEAR(actual[f][p], expected[f][p], 0.1) << f << " patch " << p;
}

TEST_F(ColorGradeExportTest, EmptyTimelineAndInvalidSnapshotStillFailAsynchronously) {
    for (const bool invalid : {false, true}) {
        ExportEngine engine; ExportRequest request;
        VideoProject empty;
        if (!invalid) request.snapshot = empty.snapshot();
        request.outputPath = utf8(root / "empty.mp4");
        std::atomic<int> finished{0};
        engine.onFinished = [&](const ExportResult&) { ++finished; };
        ASSERT_TRUE(engine.start(request)); engine.wait();
        EXPECT_EQ(engine.result().state, ExportState::Failed);
        EXPECT_EQ(engine.result().stage, "prepare");
        EXPECT_FALSE(engine.result().error.empty()); EXPECT_EQ(finished.load(), 1);
        EXPECT_FALSE(fs::exists(root / "empty.mp4"));
    }
}
