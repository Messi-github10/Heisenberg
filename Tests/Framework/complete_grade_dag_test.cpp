#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/ColorGrade.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <Compositing/Grade/GradeGpuPrograms.hpp>
#include <Compositing/Grade/LutLibrary.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
using namespace heisenberg;
using namespace heisenberg::grade;
using namespace heisenberg::renderer;
using namespace heisenberg::filtergraph;
namespace color = heisenberg::renderer::color;
namespace fs = std::filesystem;
using Pixel = std::array<float, 4>;
using RGB = std::array<float, 3>;
using Domain = ColorGradeProgramDomain;

constexpr int width = 17, height = 19;
// Red changes fastest. Cross-channel, nonaffine vertices distinguish tetrahedral
// interpolation from trilinear interpolation in every one of the six simplices.
const std::array<RGB, 8> cubeVertices{{
    {0.18f, 0.24f, 0.30f}, {0.62f, 0.22f, 0.26f},
    {0.27f, 0.58f, 0.21f}, {0.31f, 0.29f, 0.64f},
    {0.22f, 0.32f, 0.61f}, {0.67f, 0.56f, 0.28f},
    {0.34f, 0.68f, 0.49f}, {0.59f, 0.44f, 0.69f}
}};
const RGB cubeMin{0.10f, 0.18f, 0.26f}, cubeMax{0.90f, 0.82f, 0.74f};

const char* lutSpace(const std::string& domain) {
    return domain == "acescct" ? color::kGradingSpace : color::kExportDisplay;
}

GraphImageContract imageContract(const std::string& space) {
    auto contract = kWorkingImageContract;
    if (space == color::kGradingSpace) contract.transfer = filtergraph::ColorTransfer::acescct;
    else if (space == kColorGradeAuxiliarySpace) {
        contract.primaries = filtergraph::ColorPrimaries::bt709;
        contract.transfer = filtergraph::ColorTransfer::srgb;
    } else if (space == color::kExportDisplay) {
        contract.primaries = filtergraph::ColorPrimaries::bt709;
        contract.transfer = filtergraph::ColorTransfer::bt1886;
    }
    return contract;
}

JsonValue fullEffect(const LutLibrary::Snapshot& asset, const std::string& domain = "rec709") {
    auto effect = JsonValue::parse(R"({
        "id":"complete-grade","type":"video.colorGrade","schemaVersion":1,"enabled":true,
        "params":{
            "exposure":{"value":0.35},"temperature":{"value":7200},"tint":{"value":12},
            "contrast":{"value":1.08},"highlights":{"value":0.12},"shadows":{"value":-0.08},
            "blacks":{"value":0.05},"whites":{"value":-0.04},
            "vibrance":{"value":0.18},"saturation":{"value":0.92},
            "liftHue":{"value":25},"liftStrength":{"value":0.12},"liftMaster":{"value":0.015},
            "gammaHue":{"value":245},"gammaStrength":{"value":0.15},"gammaMaster":{"value":1.08},
            "gainHue":{"value":130},"gainStrength":{"value":0.12},"gainMaster":{"value":1.04},
            "curves":{
                "master":[[0,0.01],[0.19371,0.22],[0.61,0.67],[1,0.98]],
                "red":[[0,0],[0.314159,0.36],[1,1]],
                "green":[[0,0],[0.47,0.44],[1,1]],
                "blue":[[0,0],[0.53,0.57],[1,1]]
            },
            "hueTargets":[
                {"hue":355,"rotation":18,"saturation":1.2,"lightness":0.025},
                {"hue":5,"rotation":-8,"saturation":0.85,"lightness":-0.01},
                {"hue":120,"rotation":-12,"saturation":1.1,"lightness":0.015}
            ]
        }
    })");
    effect["params"]["lut"] = {{"id", asset->id}, {"name", "nonaffine"},
        {"domain", domain}, {"intensity", {{"value", 0.5}}}};
    return effect;
}

CompositionPlan makePlan(const JsonValue& effect, const LutLibrary::Snapshot& asset = {},
                         const std::string& inputSpace = color::kWorkingSpace) {
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(width, height, 24);
    plan.frameCount = 24;
    CompositionClip clip;
    clip.id = "complete-grade-clip";
    clip.mediaRef = "float-test-source";
    clip.durationFrames = 24;
    clip.visualSnapshot.id = clip.id;
    clip.visualSnapshot.mediaRef = clip.mediaRef;
    clip.visualSnapshot.durationFrames = 24;
    clip.visualSnapshot.effects = effect.is_null() ? JsonValue::array() : JsonValue::array({effect});
    plan.clips.push_back(std::move(clip));
    plan.colorSettings.inputColorSpaces["float-test-source"] = inputSpace;
    // Manually constructed plans must pin the validated asset just like
    // CompositionBuilder; the render thread must not reopen a pathname.
    if (asset) plan.gradeLuts.emplace(asset->id, asset);
    return plan;
}

std::shared_ptr<VideoFrame> floatSource(const std::vector<Pixel>& samples) {
    if (samples.empty()) return {};
    auto* raw = av_frame_alloc();
    if (!raw) return {};
    auto frame = std::shared_ptr<AVFrame>(raw, [](AVFrame* value) { av_frame_free(&value); });
    raw->width = width; raw->height = height; raw->format = AV_PIX_FMT_RGBAF32;
    raw->sample_aspect_ratio = {1, 1}; raw->color_range = AVCOL_RANGE_JPEG;
    raw->colorspace = AVCOL_SPC_RGB; raw->color_trc = AVCOL_TRC_LINEAR;
    raw->time_base = {1, 24}; raw->pts = 0;
    if (av_frame_get_buffer(raw, 32) < 0) return {};
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<float*>(raw->data[0] + y * raw->linesize[0]);
        for (int x = 0; x < width; ++x)
            std::copy(samples[size_t(x) % samples.size()].begin(),
                samples[size_t(x) % samples.size()].end(), row + x * 4);
    }
    return VideoFrame::wrap(std::move(frame), 0, Rational{24, 1});
}

Pixel pixelAt(const std::shared_ptr<AVFrame>& frame, int x, int y = 9) {
    const auto* row = reinterpret_cast<const float*>(frame->data[0] + y * frame->linesize[0]);
    return {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
}

void expectPixel(const Pixel& actual, const Pixel& expected, float tolerance = 0.008f) {
    for (size_t c = 0; c < 4; ++c) {
        EXPECT_TRUE(std::isfinite(actual[c])) << "channel " << c;
        // Production graph edges are RGBA16F, not the oracle's float32.
        EXPECT_NEAR(actual[c], expected[c], c == 3 ? 0.001f : tolerance * std::max(1.0f, std::abs(expected[c])))
            << "channel " << c;
    }
}

// Model each RGBA16F graph edge in float32, including subnormals and ties to
// even. The ACEScct inverse exponentiates its input: omitting a single half
// storage rounding can cause large linear-light differences for HDR samples.
float halfRound(float value) {
    if (!std::isfinite(value) || value == 0) return value;
    const float magnitude = std::abs(value);
    if (magnitude >= 65520.0f)
        return std::copysign(std::numeric_limits<float>::infinity(), value);
    int exponent = 0;
    std::frexp(magnitude, &exponent);
    const float step = std::ldexp(1.0f, std::max(exponent - 11, -24));
    const float scaled = magnitude / step;
    float rounded = std::floor(scaled);
    const float remainder = scaled - rounded;
    if (remainder > 0.5f || (remainder == 0.5f && std::fmod(rounded, 2.0f) != 0)) ++rounded;
    return std::copysign(rounded * step, value);
}

Pixel halfRound(Pixel pixel) {
    for (float& value : pixel) value = halfRound(value);
    return pixel;
}

void expectDecodedInterval(color::ColorPipeline& pipeline, const std::string& space,
                           const Pixel& actual, const Pixel& encoded, float encodedTolerance) {
    // These pinned decoders are monotone per-channel transfer functions followed
    // by a linear primary conversion (ACEScct is AP1 already). Evaluate all eight
    // corners, not just diagonal endpoints: matrix coefficients can be negative.
    // This propagates the same encoded error budget used by the decisive LUT
    // assertion instead of imposing an unrelated relative linear HDR tolerance.
    const auto decoder = pipeline.processor(space, color::kWorkingSpace);
    RGB lower, upper;
    lower.fill(std::numeric_limits<float>::infinity());
    upper.fill(-std::numeric_limits<float>::infinity());
    for (size_t corner = 0; corner < 8; ++corner) {
        auto pixel = encoded;
        for (size_t c = 0; c < 3; ++c)
            pixel[c] += (corner & (size_t(1) << c)) ? encodedTolerance : -encodedTolerance;
        decoder.applyRGBA(pixel.data(), 1);
        pixel = halfRound(pixel);
        for (size_t c = 0; c < 3; ++c) {
            lower[c] = std::min(lower[c], pixel[c]);
            upper[c] = std::max(upper[c], pixel[c]);
        }
    }
    for (size_t c = 0; c < 3; ++c) {
        ASSERT_TRUE(std::isfinite(lower[c]) && std::isfinite(upper[c]));
        EXPECT_TRUE(std::isfinite(actual[c]));
        // Allow one final storage ULP for CPU/GPU rounding at the interval's
        // boundary; this is float16 spacing, not an extra encoded error budget.
        int exponent = 0;
        std::frexp(std::max(std::abs(lower[c]), std::abs(upper[c])), &exponent);
        const float storageUlp = std::ldexp(1.0f, std::max(exponent - 11, -24));
        EXPECT_GE(actual[c], lower[c] - storageUlp) << "decoded interval channel " << c;
        EXPECT_LE(actual[c], upper[c] + storageUlp) << "decoded interval channel " << c;
    }
    EXPECT_NEAR(actual[3], encoded[3], 0.001f);
}

RGB normalizedInput(const LutAsset& asset, const Pixel& input) {
    RGB q{};
    for (size_t c = 0; c < 3; ++c)
        q[c] = std::clamp((input[c] - asset.metadata.domainMin[c]) /
            (asset.metadata.domainMax[c] - asset.metadata.domainMin[c]), 0.0f, 1.0f);
    return q;
}

// Independent barycentric simplex oracle. No OCIO interpolation or GPU output
// is used here, so selecting hardware trilinear filtering cannot pass the test.
RGB tetrahedron(const LutAsset& asset, const Pixel& input) {
    const auto q = normalizedInput(asset, input);
    std::array<size_t, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return q[a] > q[b]; });
    const std::array<size_t, 4> indices{0, size_t(1) << order[0],
        (size_t(1) << order[0]) | (size_t(1) << order[1]), 7};
    const std::array<float, 4> weights{1 - q[order[0]], q[order[0]] - q[order[1]],
        q[order[1]] - q[order[2]], q[order[2]]};
    RGB result{};
    for (size_t i = 0; i < 4; ++i)
        for (size_t c = 0; c < 3; ++c) result[c] += weights[i] * asset.samples[indices[i]][c];
    return result;
}

RGB trilinear(const LutAsset& asset, const Pixel& input) {
    const auto q = normalizedInput(asset, input);
    RGB result{};
    for (size_t i = 0; i < 8; ++i) {
        float weight = 1;
        for (size_t c = 0; c < 3; ++c) weight *= (i & (size_t(1) << c)) ? q[c] : 1 - q[c];
        for (size_t c = 0; c < 3; ++c) result[c] += weight * asset.samples[i][c];
    }
    return result;
}

float rgbDistance(const RGB& a, const RGB& b) {
    float distance = 0;
    for (size_t c = 0; c < 3; ++c) distance = std::max(distance, std::abs(a[c] - b[c]));
    return distance;
}

Pixel gradeOracle(color::ColorPipeline& pipeline, const JsonValue& effect,
                  const LutLibrary::Snapshot& asset, int64_t frame, Pixel pixel) {
    ColorGradeState state;
    std::vector<GradeDiagnostic> diagnostics;
    std::string error;
    if (!sampleColorGrade(effect, frame, state, diagnostics, &error)) {
        ADD_FAILURE() << error;
        return pixel;
    }
    if (!state.enabled) return pixel;
    pixel = referenceColorGradePixel(state, Domain::LinearACEScg, pixel);
    pipeline.processor(color::kWorkingSpace, color::kGradingSpace).applyRGBA(pixel.data(), 1);
    pixel = referenceColorGradePixel(state, Domain::ToneACEScct, pixel);
    pipeline.processor(color::kGradingSpace, kColorGradeAuxiliarySpace).applyRGBA(pixel.data(), 1);
    pixel = referenceColorGradePixel(state, Domain::Auxiliary, pixel);
    std::string current = kColorGradeAuxiliarySpace;
    if (asset && effect["params"].contains("lut")) {
        const auto& selection = effect["params"]["lut"];
        const auto space = std::string(lutSpace(selection["domain"].get<std::string>()));
        pipeline.processor(current, space).applyRGBA(pixel.data(), 1);
        current = space;
        // All tests below use a two-key linear intensity track when animated.
        const auto& amount = selection["intensity"];
        float intensity = amount["value"].get<float>();
        if (amount.contains("track")) {
            const auto& keys = amount["track"]["keyframes"];
            const float t = std::clamp(float(frame) / 10.0f, 0.0f, 1.0f);
            intensity = keys[0]["value"].get<float>() * (1 - t) + keys[1]["value"].get<float>() * t;
        }
        auto mapped = pixel;
        pipeline.lutProcessor(asset->metadata.size, asset->samples,
            asset->metadata.domainMin, asset->metadata.domainMax).applyRGBA(mapped.data(), 1);
        for (size_t c = 0; c < 3; ++c) pixel[c] += intensity * (mapped[c] - pixel[c]);
    }
    pipeline.processor(current, color::kWorkingSpace).applyRGBA(pixel.data(), 1);
    return pixel;
}

class CompleteGradeDagTest : public testing::Test {
protected:
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<CompositionRenderer> renderer;
    fs::path directory;
    uint64_t validationBaseline = 0;
    bool deviceReady = false;

    void SetUp() override {
        auto& shared = VulkanContext::instance();
        try {
            shared.initLoader();
            // With gtest_discover_tests this is a fresh process. For a combined
            // invocation the parent also sets VK_INSTANCE_LAYERS, since an
            // already-created singleton cannot be upgraded to validation.
            shared.createInstance(true);
            shared.createDevice();
            deviceReady = true;
            validationBaseline = shared.validationErrorCount();
            VulkanResources resources;
            resources.instance = shared.vkInstance(); resources.physDevice = shared.physicalDevice();
            resources.device = shared.device(); resources.graphicsQF = shared.graphicsQueueFamily();
            resources.graphicsQueue = shared.graphicsQueue(); resources.getProcAddr = shared.getInstanceProcAddr();
            resources.enabledFeatures = shared.enabledDeviceFeatures();
            resources.enabledExtensions = shared.enabledDeviceExtensions();
            gpu = std::make_unique<GpuContext>(resources);
        } catch (const std::exception& exception) {
            GTEST_SKIP() << "No validation-enabled windowless Vulkan GPU: " << exception.what();
        }
        ASSERT_NE(gpu->plVulkan(), nullptr);
        renderer = std::make_unique<CompositionRenderer>();
        std::string error;
        ASSERT_TRUE(renderer->initialize(gpu->plVulkan()->gpu, gpu->plVulkan(), nullptr, nullptr, &error)) << error;
        static std::atomic<unsigned> sequence{0};
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = fs::temp_directory_path() / ("heisenberg-complete-grade-" +
                std::to_string(random()) + "-" + std::to_string(sequence++));
            if (fs::create_directory(candidate)) { directory = std::move(candidate); break; }
        }
        ASSERT_FALSE(directory.empty());
    }

    void TearDown() override {
        // Include graph destruction, fence draining and libplacebo teardown in
        // the validation assertion, not just successful dispatches.
        renderer.reset();
        gpu.reset();
        if (deviceReady) EXPECT_EQ(VulkanContext::instance().validationErrorCount(), validationBaseline);
        LutLibrary::setDefaultRoot({});
        if (!directory.empty()) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
    }

    LutLibrary::Snapshot importCube() {
        const auto source = directory / "source.cube";
        std::ofstream stream(source, std::ios::binary | std::ios::trunc);
        stream.imbue(std::locale::classic());
        stream << std::setprecision(9) << "TITLE \"Nonaffine tetrahedral test\"\nLUT_3D_SIZE 2\nDOMAIN_MIN";
        for (float value : cubeMin) stream << ' ' << value;
        stream << "\nDOMAIN_MAX";
        for (float value : cubeMax) stream << ' ' << value;
        stream << '\n';
        for (const auto& vertex : cubeVertices) stream << vertex[0] << ' ' << vertex[1] << ' ' << vertex[2] << '\n';
        stream.close();
        EXPECT_TRUE(stream.good());
        LutLibrary library(directory / "managed-root");
        std::string error;
        auto asset = library.importFile(source, &error);
        EXPECT_NE(asset, nullptr) << error;
        return asset;
    }

    std::shared_ptr<AVFrame> renderFloat(const CompositionPlan& plan,
        const std::shared_ptr<VideoFrame>& source, int64_t frame = 0) {
        std::string error;
        auto rendered = renderer->render(plan, frame, source, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered) return {};
        EXPECT_FALSE(rendered.encoded());
        EXPECT_EQ(rendered.image().contract, kWorkingImageContract);
        auto result = renderer->download(rendered, DownloadFormat::RgbaFloat, &error);
        EXPECT_NE(result, nullptr) << error;
        if (result) EXPECT_EQ(result->format, AV_PIX_FMT_RGBAF32);
        return result;
    }

    bool compile(const CompositionPlan& plan, int64_t frame, CompiledGrade& result, std::string& error) {
        return GradeCompiler::compile(plan.clips.front().visualSnapshot, frame, result, &error,
            {.strict = true}, nullptr, &plan.gradeLuts);
    }

    void expectCompleteChain(const CompiledGrade& compiled, const std::string& domain,
                             color::ColorPipeline& pipeline) {
        const auto& document = compiled.document;
        std::string error;
        ASSERT_TRUE(document.validate(&error)) << error;
        ASSERT_EQ(document.nodes().size(), 10u); // Two endpoints, four grades, four conversions.
        ASSERT_EQ(document.edges().size(), 9u);
        ASSERT_EQ(compiled.programUpdates.size(), 8u);
        EXPECT_TRUE(compiled.parameterUpdates.empty());
        EXPECT_EQ(std::count_if(document.nodes().begin(), document.nodes().end(),
            [](const auto& node) { return node.filterId == "input"; }), 1);
        EXPECT_EQ(std::count_if(document.nodes().begin(), document.nodes().end(),
            [](const auto& node) { return node.filterId == "output"; }), 1);
        const std::string lutDomain = lutSpace(domain);
        const std::array<std::pair<std::string, std::string>, 4> conversions{{
            {color::kWorkingSpace, color::kGradingSpace},
            {color::kGradingSpace, kColorGradeAuxiliarySpace},
            {kColorGradeAuxiliarySpace, lutDomain}, {lutDomain, color::kWorkingSpace}
        }};
        const std::array<const char*, 4> functions{
            "colorGradeLinear", "colorGradeTone", "colorGradeAuxiliary", "heisenbergCreativeLut"};
        const std::array<std::string, 4> gradeSpaces{
            color::kWorkingSpace, color::kGradingSpace, kColorGradeAuxiliarySpace, lutDomain};
        auto previous = kVulkanGraphInputNodeId;
        for (size_t i = 0; i < 8; ++i) {
            const auto edge = std::find_if(document.edges().begin(), document.edges().end(),
                [&](const auto& candidate) { return candidate.output.nodeId == previous; });
            ASSERT_NE(edge, document.edges().end());
            EXPECT_EQ(edge->output.pinIndex, 0); EXPECT_EQ(edge->input.pinIndex, 0);
            const auto node = std::find_if(document.nodes().begin(), document.nodes().end(),
                [&](const auto& candidate) { return candidate.id == edge->input.nodeId; });
            ASSERT_NE(node, document.nodes().end()); ASSERT_NE(node->program, nullptr);
            EXPECT_EQ(node->filterId, "program");
            if (i == 0 || i == 2 || i == 4 || i == 6) {
                const size_t grade = i / 2;
                EXPECT_EQ(node->program->functionName, functions[grade]);
                EXPECT_EQ(node->inputContract, imageContract(gradeSpaces[grade]));
                EXPECT_EQ(node->outputContract, imageContract(gradeSpaces[grade]));
                if (grade == 3) {
                    ASSERT_FALSE(node->program->textures().empty());
                    EXPECT_TRUE(std::any_of(node->program->textures().begin(), node->program->textures().end(),
                        [](const auto& lut) { return lut.is3D && !lut.linearInterpolation; }));
                }
            } else {
                const auto& conversion = conversions[i / 2];
                const auto expected = pipeline.processor(conversion.first, conversion.second).gpuProgramSnapshot();
                EXPECT_EQ(node->program->cacheId, expected->cacheId);
                EXPECT_EQ(node->inputContract, imageContract(conversion.first));
                EXPECT_EQ(node->outputContract, imageContract(conversion.second));
            }
            previous = node->id;
        }
        EXPECT_EQ(std::count_if(document.edges().begin(), document.edges().end(), [&](const auto& edge) {
            return edge.output.nodeId == previous && edge.input.nodeId == kVulkanGraphOutputNodeId;
        }), 1);
        // The sole last conversion returns to linear AP1, not a display/view.
        EXPECT_EQ(document.nodes().back().outputContract, kWorkingImageContract);
    }
};

TEST_F(CompleteGradeDagTest, AllGradeDomainsAndPinnedLutMatchBracketedCpuOracle) {
    auto asset = importCube(); ASSERT_NE(asset, nullptr);
    color::ColorPipeline pipeline;
    const auto spaces = pipeline.colorSpaces();
    ASSERT_NE(std::find(spaces.begin(), spaces.end(), kColorGradeAuxiliarySpace), spaces.end())
        << "The declared auxiliary space must exist in the pinned studio config";
    // RGB probes are opaque: canvas compositing precedes grading, and would
    // otherwise attenuate these inputs before they reach the complete DAG.
    const std::vector<Pixel> samples{{0.22f, 0.04f, 0.035f, 1}, {0.06f, 0.25f, 0.08f, 1},
        {0.04f, 0.08f, 0.3f, 1}, {0.18f, 0.18f, 0.18f, 1}, {-0.025f, 0.2f, 1.7f, 1}};
    auto source = floatSource(samples); ASSERT_TRUE(source);
    auto baseline = renderFloat(makePlan(nullptr), source); ASSERT_NE(baseline, nullptr);
    // Eliminate both paths before the first shader extraction, not merely after
    // warming the program cache. Only the plan's validated samples can work.
    ASSERT_TRUE(fs::remove(directory / "source.cube"));
    ASSERT_TRUE(fs::remove(asset->resourcePath));
    LutLibrary::setDefaultRoot(directory / "unrelated-empty-root");
    for (const auto& domain : {std::string("rec709"), std::string("acescct")}) {
        SCOPED_TRACE(domain);
        const auto effect = fullEffect(asset, domain);
        auto plan = makePlan(effect, asset);
        CompiledGrade compiled; std::string error;
        ASSERT_TRUE(compile(plan, 0, compiled, error)) << error;
        expectCompleteChain(compiled, domain, pipeline);
        for (int repeat = 0; repeat < 2; ++repeat) {
            auto actual = renderFloat(plan, source); ASSERT_NE(actual, nullptr);
            for (int x = 0; x < width; ++x) {
                SCOPED_TRACE(x);
                const auto expected = gradeOracle(pipeline, effect, asset, 0, pixelAt(baseline, x));
                for (int y : {0, 9, height - 1}) expectPixel(pixelAt(actual, x, y), expected);
                EXPECT_NEAR(pixelAt(actual, x)[3], pixelAt(baseline, x)[3], 0.001f);
            }
        }
    }
}

TEST_F(CompleteGradeDagTest, TetrahedralSixSimplicesDomainBoundsIntensityAndAlpha) {
    auto asset = importCube(); ASSERT_NE(asset, nullptr);
    ASSERT_EQ(asset->metadata.size, 2u);
    EXPECT_EQ(asset->metadata.domainMin, cubeMin); EXPECT_EQ(asset->metadata.domainMax, cubeMax);
    color::ColorPipeline pipeline;
    std::vector<Pixel> samples;
    RGB permutation{0.20f, 0.47f, 0.76f};
    do {
        // Keep every probe opaque so its coordinates remain inside the intended
        // simplex after the renderer's black-canvas composition step.
        Pixel pixel{0, 0, 0, 1};
        for (size_t c = 0; c < 3; ++c) pixel[c] = cubeMin[c] + permutation[c] * (cubeMax[c] - cubeMin[c]);
        ASSERT_GT(rgbDistance(tetrahedron(*asset, pixel), trilinear(*asset, pixel)), 0.009f);
        samples.push_back(pixel);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    ASSERT_EQ(samples.size(), 6u);
    samples.push_back({cubeMin[0] - 0.12f, cubeMax[1] + 0.12f, 0.5f, 1});
    samples.push_back({cubeMax[0] + 0.12f, cubeMin[1] - 0.12f, 0.4f, 1});
    for (const auto& domain : {std::string("rec709"), std::string("acescct")}) {
        SCOPED_TRACE(domain);
        const auto space = std::string(lutSpace(domain));
        auto source = floatSource(samples); ASSERT_TRUE(source);
        auto baseline = renderFloat(makePlan(nullptr, {}, space), source); ASSERT_NE(baseline, nullptr);
        for (float intensity : {0.0f, 0.5f, 1.0f, 0.5f}) {
            SCOPED_TRACE(intensity);
            auto effect = JsonValue{{"type", kColorGradeType}, {"schemaVersion", 1}, {"params", {
                {"lut", {{"id", asset->id}, {"domain", domain}, {"intensity", {{"value", intensity}}}}}
            }}};
            auto plan = makePlan(effect, asset, space);
            CompiledGrade compiled; std::string error;
            ASSERT_TRUE(compile(plan, 0, compiled, error)) << error;
            EXPECT_EQ(compiled.neutral(), intensity == 0);
            if (intensity > 0) {
                ASSERT_EQ(compiled.document.nodes().size(), 5u); // encode, LUT, decode + endpoints.
                ASSERT_EQ(compiled.programUpdates.size(), 3u);
            }
            auto actual = renderFloat(plan, source); ASSERT_NE(actual, nullptr);
            for (int x = 0; x < width; ++x) {
                SCOPED_TRACE(x);
                auto input = pixelAt(baseline, x);
                if (intensity == 0) {
                    expectPixel(pixelAt(actual, x), input, 0.0001f);
                    continue;
                }
                pipeline.processor(color::kWorkingSpace, space).applyRGBA(input.data(), 1);
                input = halfRound(input); // OCIO encode program's RGBA16F output.
                const auto mapped = tetrahedron(*asset, input);
                if (size_t(x) % samples.size() < 6)
                    EXPECT_GT(rgbDistance(mapped, trilinear(*asset, input)), 0.008f);
                // Verify OCIO's in-memory CPU path independently as well as GPU.
                auto ocio = input;
                pipeline.lutProcessor(2, asset->samples, cubeMin, cubeMax).applyRGBA(ocio.data(), 1);
                for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(ocio[c], mapped[c], 2e-6f);
                Pixel expectedDomain = input;
                for (size_t c = 0; c < 3; ++c) expectedDomain[c] += intensity * (mapped[c] - input[c]);
                expectedDomain = halfRound(expectedDomain); // LUT mix program's storage edge.
                constexpr float encodedTolerance = 0.002f;
                expectDecodedInterval(pipeline, space, pixelAt(actual, x), expectedDomain, encodedTolerance);
                // The exact tetrahedral oracle remains decisive in the declared
                // encoded domain. ACEScct's inverse derivative scales as
                // ln(2)*17.52*linearValue: +/-0.002 becomes about +/-2.46% in HDR,
                // even before primary mixing and half-float storage rounding.
                auto actualDomain = pixelAt(actual, x);
                pipeline.processor(color::kWorkingSpace, space).applyRGBA(actualDomain.data(), 1);
                expectPixel(actualDomain, expectedDomain, encodedTolerance);
                if (intensity == 1 && size_t(x) % samples.size() < 6) {
                    const RGB actualRgb{actualDomain[0], actualDomain[1], actualDomain[2]};
                    EXPECT_GT(rgbDistance(actualRgb, trilinear(*asset, input)), 0.006f)
                        << "GPU must reject trilinear interpolation in every simplex";
                    const auto q = normalizedInput(*asset, input);
                    const auto& original = samples[size_t(x) % samples.size()];
                    const auto originalQ = normalizedInput(*asset, original);
                    for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(q[c], originalQ[c], 0.003f);
                }
                EXPECT_NEAR(pixelAt(actual, x)[3], pixelAt(baseline, x)[3], 0.001f);
                // DOMAIN_* is an input normalization, not output rescaling.
                Pixel wrong = input;
                for (size_t c = 0; c < 3; ++c) wrong[c] = std::clamp(input[c], 0.0f, 1.0f);
                LutAsset unitDomain = *asset;
                unitDomain.metadata.domainMin = {0, 0, 0}; unitDomain.metadata.domainMax = {1, 1, 1};
                if (x == 0) EXPECT_GT(rgbDistance(mapped, tetrahedron(unitDomain, wrong)), 0.01f);
            }
        }
    }
}

TEST_F(CompleteGradeDagTest, AnimationChangesOnlyUniformsAndWarmRendererMatchesEachFrame) {
    auto asset = importCube(); ASSERT_NE(asset, nullptr);
    auto effect = fullEffect(asset);
    auto track = [](float a, float b) {
        return JsonValue{{"keyframes", JsonValue::array({
            {{"frame", 0}, {"value", a}, {"interpolationOut", "linear"}}, {{"frame", 10}, {"value", b}}
        })}};
    };
    effect["params"]["exposure"]["track"] = track(0.2f, 0.6f);
    effect["params"]["contrast"]["track"] = track(0.92f, 1.15f);
    effect["params"]["saturation"]["track"] = track(0.75f, 1.15f);
    effect["params"]["gammaMaster"]["track"] = track(0.85f, 1.2f);
    effect["params"]["hueTargets"][0]["rotation"] = {{"value", 18}, {"track", track(-10, 20)}};
    effect["params"]["lut"]["intensity"]["track"] = track(0.25f, 0.75f);
    auto plan = makePlan(effect, asset);
    CompiledGrade first; std::string error;
    ASSERT_TRUE(compile(plan, 0, first, error)) << error;
    auto source = floatSource({{0.22f, 0.04f, 0.035f, 1}, {0.06f, 0.25f, 0.08f, 1},
        {-0.02f, 0.18f, 1.5f, 1}}); ASSERT_TRUE(source);
    auto baseline = renderFloat(makePlan(nullptr), source); ASSERT_NE(baseline, nullptr);
    color::ColorPipeline pipeline;
    for (int64_t frame : {0, 5, 10, 5, 0, 10}) {
        SCOPED_TRACE(frame);
        CompiledGrade sampled;
        ASSERT_TRUE(compile(plan, frame, sampled, error)) << error;
        EXPECT_EQ(sampled.topologyKey, first.topologyKey);
        EXPECT_EQ(sampled.document.edges(), first.document.edges());
        ASSERT_EQ(sampled.programUpdates.size(), first.programUpdates.size());
        size_t changedUniforms = 0;
        for (size_t i = 0; i < sampled.programUpdates.size(); ++i) {
            const auto& a = *first.programUpdates[i].program;
            const auto& b = *sampled.programUpdates[i].program;
            EXPECT_EQ(sampled.programUpdates[i].nodeId, first.programUpdates[i].nodeId);
            EXPECT_EQ(a.cacheId, b.cacheId); EXPECT_EQ(a.shaderText, b.shaderText);
            EXPECT_EQ(a.uniformData.size(), b.uniformData.size());
            changedUniforms += a.uniformData != b.uniformData;
            const auto& oldTextures = a.textures();
            const auto& newTextures = b.textures();
            ASSERT_EQ(oldTextures.size(), newTextures.size());
            for (size_t j = 0; j < oldTextures.size(); ++j) {
                EXPECT_EQ(oldTextures[j].binding, newTextures[j].binding);
                EXPECT_EQ(oldTextures[j].width, newTextures[j].width);
                EXPECT_EQ(oldTextures[j].height, newTextures[j].height);
                EXPECT_EQ(oldTextures[j].depth, newTextures[j].depth);
                EXPECT_EQ(oldTextures[j].channels, newTextures[j].channels);
                EXPECT_EQ(oldTextures[j].is3D, newTextures[j].is3D);
                EXPECT_EQ(oldTextures[j].linearInterpolation, newTextures[j].linearInterpolation);
                EXPECT_EQ(oldTextures[j].values, newTextures[j].values);
            }
        }
        if (frame != 0) EXPECT_GE(changedUniforms, 4u); // Linear, tone, auxiliary and LUT intensity.
        auto actual = renderFloat(plan, source, frame); ASSERT_NE(actual, nullptr);
        for (int x = 0; x < width; ++x)
            expectPixel(pixelAt(actual, x), gradeOracle(pipeline, effect, asset, frame, pixelAt(baseline, x)));
    }
    // Immutable curve data must invalidate the same cache that ignores UBO edits.
    auto curveEdit = plan;
    (*curveEdit.clips.front().visualSnapshot.effects)[0]["params"]["curves"]["red"][1][1] = 0.4;
    CompiledGrade changed;
    ASSERT_TRUE(compile(curveEdit, 0, changed, error)) << error;
    EXPECT_NE(changed.topologyKey, first.topologyKey);
}

TEST_F(CompleteGradeDagTest, MissingPinnedLutFailsStrictPreviewDiagnosesAndZeroNeedsNoResource) {
    auto asset = importCube(); ASSERT_NE(asset, nullptr);
    LutLibrary::setDefaultRoot(asset->resourcePath.parent_path());
    ASSERT_NE(LutLibrary::resolveConfigured(asset->id), nullptr);
    auto effect = fullEffect(asset);
    auto plan = makePlan(effect); // Deliberately absent from the plan, though available globally.
    plan.gradeLutErrors[asset->id] = "missing in captured composition";
    auto source = floatSource({{0.22f, 0.04f, 0.035f, 1}}); ASSERT_TRUE(source);
    std::string error;
    CompiledGrade compiled;
    EXPECT_FALSE(compile(plan, 0, compiled, error));
    EXPECT_NE(error.find(asset->id), std::string::npos);
    CompositionRenderRequest request;
    request.plan = &plan; request.source = source; request.clipId = plan.clips.front().id;
    request.strict = true;
    EXPECT_FALSE(renderer->render(request, &error));
    EXPECT_NE(error.find(asset->id), std::string::npos);
    std::vector<std::string> diagnostics;
    request.strict = false; request.diagnostics = &diagnostics;
    auto preview = renderer->render(request, &error); ASSERT_TRUE(preview) << error;
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& message) {
        return message.find(asset->id) != std::string::npos;
    }));
    auto actual = renderer->download(preview, DownloadFormat::RgbaFloat, &error); ASSERT_NE(actual, nullptr) << error;
    auto baseline = renderFloat(makePlan(nullptr), source); ASSERT_NE(baseline, nullptr);
    auto withoutLut = effect; withoutLut["params"].erase("lut");
    color::ColorPipeline pipeline;
    expectPixel(pixelAt(actual, 8), gradeOracle(pipeline, withoutLut, {}, 0, pixelAt(baseline, 8)));
    // A nonexistent identity must also be harmless at exactly zero intensity.
    auto zero = effect;
    zero["params"]["lut"]["id"] = std::string(64, 'a');
    zero["params"]["lut"]["intensity"]["value"] = 0;
    auto zeroPlan = makePlan(zero);
    ASSERT_TRUE(compile(zeroPlan, 0, compiled, error)) << error;
    auto zeroOutput = renderFloat(zeroPlan, source); ASSERT_NE(zeroOutput, nullptr);
    expectPixel(pixelAt(zeroOutput, 8), pixelAt(actual, 8), 0.0001f);
}

TEST_F(CompleteGradeDagTest, NeutralDisabledAndLinearGradePreserveNegativeHdrAndStraightAlpha) {
    const std::vector<Pixel> samples{{-0.125f, 0.25f, 2.0f, 1}, {0.5f, -0.25f, 1.5f, 1}};
    auto source = floatSource(samples); ASSERT_TRUE(source);
    auto baseline = renderFloat(makePlan(nullptr), source); ASSERT_NE(baseline, nullptr);
    auto effect = JsonValue{{"type", kColorGradeType}, {"schemaVersion", 1}, {"params", JsonValue::object()}};
    auto plan = makePlan(effect);
    CompiledGrade compiled; std::string error;
    ASSERT_TRUE(compile(plan, 0, compiled, error)) << error; EXPECT_TRUE(compiled.neutral());
    auto neutral = renderFloat(plan, source); ASSERT_NE(neutral, nullptr);
    effect["params"]["exposure"] = {{"value", 1}};
    plan = makePlan(effect);
    ASSERT_TRUE(compile(plan, 0, compiled, error)) << error;
    ASSERT_EQ(compiled.programUpdates.size(), 1u);
    EXPECT_EQ(compiled.programUpdates.front().program->functionName, "colorGradeLinear");
    auto graded = renderFloat(plan, source); ASSERT_NE(graded, nullptr);
    effect["enabled"] = false;
    auto disabled = renderFloat(makePlan(effect), source); ASSERT_NE(disabled, nullptr);
    for (int x = 0; x < width; ++x) {
        auto expected = pixelAt(baseline, x);
        expectPixel(pixelAt(neutral, x), expected, 0.0001f);
        expectPixel(pixelAt(disabled, x), expected, 0.0001f);
        EXPECT_NEAR(pixelAt(graded, x)[3], expected[3], 0.001f);
        for (size_t c = 0; c < 3; ++c) expected[c] *= 2;
        expectPixel(pixelAt(graded, x), expected, 0.002f);
        EXPECT_LT(pixelAt(graded, x)[size_t(x) % samples.size() == 0 ? 0 : 1], 0);
        EXPECT_GT(pixelAt(graded, x)[2], 1);
    }
    // Fractional source alpha is composited before grading in this renderer.
    // Verify that enabling the DAG does not change that established output
    // alpha; raw straight-alpha preservation is covered by ColorGradeGpuTest
    // and GpuProgramGraphTest, without the canvas compositing stage.
    auto translucentSource = floatSource({{0.2f, 0.3f, 0.4f, 0.37f}});
    ASSERT_TRUE(translucentSource);
    auto translucentBaseline = renderFloat(makePlan(nullptr), translucentSource);
    ASSERT_NE(translucentBaseline, nullptr);
    effect["enabled"] = true;
    auto translucentGrade = renderFloat(makePlan(effect), translucentSource);
    ASSERT_NE(translucentGrade, nullptr);
    for (int x = 0; x < width; ++x)
        EXPECT_NEAR(pixelAt(translucentGrade, x)[3], pixelAt(translucentBaseline, x)[3], 0.001f);
}
} // namespace
