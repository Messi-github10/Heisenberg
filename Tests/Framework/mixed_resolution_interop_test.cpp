#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Utiles/Logger.hpp>
#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <d3d11.h>

extern "C" {
#include <libavutil/buffer.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
using Pixel = std::array<float,4>;
constexpr int kPoolSlots = 6;
constexpr int kWarmFrames = 20;
constexpr const char* kInputSpace = "Linear Rec.709 (sRGB)";

struct ResourceCounts {
    size_t initializations = 0;
    size_t imports = 0;
};

// Observe the existing INFO diagnostics, without adding production counters.
class InteropLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    ResourceCounts counts() {
        std::lock_guard lock(mutex_);
        return counts_;
    }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level != spdlog::level::info) return;
        const std::string_view text(message.payload.data(),message.payload.size());
        if (text.starts_with("D3D11VulkanInterop: initialized ")) ++counts_.initializations;
        if (text == "D3D11VulkanInterop: Vulkan shared image import succeeded") ++counts_.imports;
    }
    void flush_() override {}
    ResourceCounts counts_;
};

class CaptureInteropLogs {
public:
    CaptureInteropLogs() {
        logger_ = Logger::ModuleLogger("Framework/Platform/D3D11/D3D11VulkanInterop.cpp");
        previousSinks_ = logger_->sinks();
        previousLevel_ = logger_->level();
        // Keep console/file logging overhead out of the baseline comparison.
        // Restore the complete logger state at the end of this individual test.
        logger_->sinks() = {sink_};
        logger_->set_level(spdlog::level::info);
    }
    ~CaptureInteropLogs() {
        logger_->sinks() = std::move(previousSinks_);
        logger_->set_level(previousLevel_);
    }
    ResourceCounts counts() const { return sink_->counts(); }
    void expect(const char* phase, size_t initializations, size_t imports) const {
        const auto value = counts();
        std::cout << "[interop resources] " << phase << ": initializations="
                  << value.initializations << " shared_import_successes=" << value.imports << '\n';
        // Deliberately nonfatal: the pre-cache renderer must still emit timing.
        EXPECT_EQ(value.initializations,initializations) << phase;
        EXPECT_EQ(value.imports,imports) << phase;
    }
private:
    spdlog::logger* logger_ = nullptr;
    std::vector<spdlog::sink_ptr> previousSinks_;
    spdlog::level::level_enum previousLevel_ = spdlog::level::info;
    std::shared_ptr<InteropLogSink> sink_ = std::make_shared<InteropLogSink>();
};

struct Source {
    std::shared_ptr<VideoFrame> frame;
    Pixel unpacked{};
};

Pixel rec709Codes(int y, int cb, int cr) {
    const float luma = float(y-16)/219.0f;
    const float u = float(cb-128)/224.0f, v = float(cr-128)/224.0f;
    constexpr float kr = 0.2126f, kb = 0.0722f, kg = 1.0f-kr-kb;
    return {luma+2.0f*(1.0f-kr)*v,
            luma-2.0f*kb*(1.0f-kb)/kg*u-2.0f*kr*(1.0f-kr)/kg*v,
            luma+2.0f*(1.0f-kb)*u,1.0f};
}

Source hardwareSource(ID3D11Device* device, int width, int height, DXGI_FORMAT format,
                      std::array<int,3> codes, std::string& error) {
    const bool rgba = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool tenBit = format == DXGI_FORMAT_P010;
    const unsigned pitch = unsigned(width)*(rgba ? 8u : tenBit ? 2u : 1u);
    const size_t bytes = size_t(pitch)*(rgba ? height : height*3/2);
    std::vector<uint8_t> packed(bytes);
    Pixel expected = rgba ? Pixel{0.25f,0.5f,0.75f,1.0f} : rec709Codes(codes[0],codes[1],codes[2]);
    if (rgba) {
        constexpr std::array<uint16_t,4> halfPixel{0x3400,0x3800,0x3a00,0x3c00};
        for (size_t i = 0; i < packed.size(); i += sizeof(halfPixel))
            std::memcpy(packed.data()+i,halfPixel.data(),sizeof(halfPixel));
    } else {
        auto store = [&](size_t sample, int code) {
            if (tenBit) {
                const uint16_t word = static_cast<uint16_t>((code*4) << 6);
                std::memcpy(packed.data()+sample*2,&word,sizeof(word));
            } else packed[sample] = static_cast<uint8_t>(code);
        };
        const size_t lumaSamples = size_t(width)*height;
        for (size_t i = 0; i < lumaSamples; ++i) store(i,codes[0]);
        for (size_t i = lumaSamples; i < lumaSamples*3/2; i += 2) {
            store(i,codes[1]); store(i+1,codes[2]);
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = packed.data(); data.SysMemPitch = pitch;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT status = device->CreateTexture2D(&desc,&data,&texture);
    if (FAILED(status)) {
        error = "CreateTexture2D format="+std::to_string(int(format))+" HRESULT="+std::to_string(unsigned(status));
        return {};
    }
    auto* raw = av_frame_alloc();
    if (!raw) { texture->Release(); error = "av_frame_alloc failed"; return {}; }
    auto owned = std::shared_ptr<AVFrame>(raw,[](AVFrame* p) { av_frame_free(&p); });
    // Make both AVFrame clones and VideoFrame copies retain the COM resource.
    raw->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(texture),1,
        [](void* opaque, uint8_t*) { static_cast<ID3D11Texture2D*>(opaque)->Release(); },
        texture,AV_BUFFER_FLAG_READONLY);
    if (!raw->buf[0]) { texture->Release(); error = "av_buffer_create failed"; return {}; }
    raw->data[0] = reinterpret_cast<uint8_t*>(texture);
    raw->data[1] = nullptr; // Texture array slice zero.
    raw->width = width; raw->height = height; raw->format = AV_PIX_FMT_D3D11;
    raw->sample_aspect_ratio = {1,1}; raw->time_base = {1,24}; raw->pts = 37;
    raw->color_primaries = AVCOL_PRI_BT709;
    raw->color_trc = rgba ? AVCOL_TRC_LINEAR : AVCOL_TRC_BT709;
    raw->colorspace = rgba ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
    raw->color_range = rgba ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    raw->chroma_location = AVCHROMA_LOC_LEFT;
    return {VideoFrame::wrap(std::move(owned),0,Rational{24,1}),expected};
}

CompositionRenderRequest requestFor(const std::vector<Source>& sources, int width, int height) {
    CompositionRenderRequest request;
    request.settings = TimelineSettings::fromTimelineSettings(width,height,24);
    // Explicit IDT makes the independent unpacked RGB values a linear blend
    // oracle; metadata fallback and output tone mapping cannot hide stale state.
    request.inputColorSpace = kInputSpace;
    request.sourceId = "synthetic-mixed-resolution";
    request.strict = true;
    for (size_t i = 0; i < sources.size(); ++i)
        request.layers.push_back({sources[i].frame,"track-"+std::to_string(i),
                                  "clip-"+std::to_string(i),0,0.5});
    return request;
}

Pixel compositePixel(const std::vector<Source>& sources) {
    Pixel result{0,0,0,1};
    // Requests are top first, with every layer visible at opacity 0.5.
    for (auto it = sources.rbegin(); it != sources.rend(); ++it)
        for (size_t c = 0; c < 3; ++c) result[c] = 0.5f*(it->unpacked[c]+result[c]);
    return result;
}

class MixedResolutionInteropTest : public testing::Test {
protected:
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<CompositionRenderer> renderer;
    ID3D11Device* device = nullptr;
    uint64_t validationBaseline = 0;
    bool contextReady = false;
    std::string error;
    int64_t position = 0;

    void SetUp() override {
        auto& vk = VulkanContext::instance();
        try {
            vk.initLoader(); vk.createInstance(true); vk.createDevice();
            contextReady = true; validationBaseline = vk.validationErrorCount();
            D3D11Context::instance().createDevice();
            device = D3D11Context::instance().device();
            if (!device) GTEST_SKIP() << "No D3D11 hardware device";
            VulkanResources resources;
            resources.instance = vk.vkInstance(); resources.physDevice = vk.physicalDevice();
            resources.device = vk.device(); resources.graphicsQF = vk.graphicsQueueFamily();
            resources.graphicsQueue = vk.graphicsQueue(); resources.getProcAddr = vk.getInstanceProcAddr();
            resources.enabledFeatures = vk.enabledDeviceFeatures();
            resources.enabledExtensions = vk.enabledDeviceExtensions();
            gpu = std::make_unique<GpuContext>(resources);
        } catch (const std::exception& e) { GTEST_SKIP() << "No windowless interop GPU: " << e.what(); }
        renderer = std::make_unique<CompositionRenderer>();
        ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),device,
                                        D3D11Context::instance().context(),&error)) << error;
    }
    void TearDown() override {
        renderer.reset(); gpu.reset();
        if (contextReady) EXPECT_EQ(VulkanContext::instance().validationErrorCount(),validationBaseline);
    }
    bool run(CompositionRenderRequest request, int frames) {
        for (int i = 0; i < frames; ++i) {
            request.targetFrame = position++;
            auto rendered = renderer->render(request,&error);
            if (!rendered) { ADD_FAILURE() << error; return false; }
        }
        return true;
    }
    bool timed(const char* phase, CompositionRenderRequest request, bool rotate = false) {
        std::vector<double> samples;
        samples.reserve(kWarmFrames);
        bool success = true;
        for (int i = 0; i < kWarmFrames; ++i) {
            if (rotate && request.layers.size() > 1)
                std::rotate(request.layers.begin(),request.layers.begin()+1,request.layers.end());
            if (rotate && i%2) std::reverse(request.layers.begin(),request.layers.end());
            request.targetFrame = position++;
            const auto start = std::chrono::steady_clock::now();
            auto rendered = renderer->render(request,&error);
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double,std::milli>(end-start).count());
            if (!rendered) { ADD_FAILURE() << error; success = false; break; }
            // Final lease destruction is outside the API interval, as are all
            // readbacks, source creation, logging assertions, and request setup.
        }
        std::sort(samples.begin(),samples.end());
        const double average = std::accumulate(samples.begin(),samples.end(),0.0)/samples.size();
        const size_t p95 = size_t(std::ceil(0.95*double(samples.size())))-1;
        std::cout << "[interop timing] " << phase << " CPU/API render wall time (not GPU time)"
                  << " frames=" << samples.size() << " avg_ms=" << average
                  << " p95_ms=" << samples[p95] << " max_ms=" << samples.back() << std::endl;
        return success; // No hardware-dependent timing threshold.
    }
    void snapshot(const std::vector<Source>& sources) {
        // A small readback oracle, never a readback of every timed HD frame.
        auto request = requestFor(sources,32,18); request.targetFrame = position++;
        auto working = renderer->render(request,&error); ASSERT_TRUE(working) << error;
        EXPECT_EQ(working.image().contract,filtergraph::kWorkingImageContract);
        ColorOutputRequest selection; selection.colorSpace = kInputSpace;
        auto output = renderer->output(working,selection,&error); ASSERT_TRUE(output) << error;
        auto pixels = renderer->download(output,DownloadFormat::RgbaFloat,&error);
        ASSERT_TRUE(pixels) << error;
        ASSERT_EQ(pixels->width,32); ASSERT_EQ(pixels->height,18);
        const auto expected = compositePixel(sources);
        for (const auto point : {std::array<int,2>{8,4},{24,4},{16,9},{8,13},{24,13}}) {
            const auto* actual = reinterpret_cast<const float*>(pixels->data[0]+point[1]*pixels->linesize[0])+point[0]*4;
            for (size_t c = 0; c < 4; ++c) {
                EXPECT_TRUE(std::isfinite(actual[c]));
                EXPECT_NEAR(actual[c],expected[c],0.008f)
                    << "x=" << point[0] << " y=" << point[1] << " channel=" << c;
            }
        }
    }
};

TEST_F(MixedResolutionInteropTest, HdAndTwo768LayersStayWarmAcrossOneThreeOneAndReentry) {
    CaptureInteropLogs logs;
    logs.expect("fresh test",0,0);
    const auto hd = hardwareSource(device,1920,1080,DXGI_FORMAT_NV12,{64,100,180},error);
    ASSERT_TRUE(hd.frame) << error;
    const auto smallA = hardwareSource(device,768,432,DXGI_FORMAT_NV12,{128,160,100},error);
    ASSERT_TRUE(smallA.frame) << error;
    const auto smallB = hardwareSource(device,768,432,DXGI_FORMAT_NV12,{192,90,140},error);
    ASSERT_TRUE(smallB.frame) << error;
    const std::vector<Source> single{hd}, mixed{hd,smallA,smallB};
    const auto one = requestFor(single,1920,1080), three = requestFor(mixed,1920,1080);

    ASSERT_TRUE(run(one,kPoolSlots));
    logs.expect("single prewarm: six slots",1,kPoolSlots);
    ASSERT_TRUE(timed("1 layer 1920x1080",one));
    logs.expect("single warm",1,kPoolSlots);
    ASSERT_TRUE(run(three,kPoolSlots));
    // The two distinct 768x432 textures must share ONE extent cache and its
    // six slots. The HD cache remains alive: two initializations, twelve imports.
    logs.expect("mixed prewarm: two extents",2,2*kPoolSlots);
    ASSERT_TRUE(timed("3 layers 1920x1080 + 768x432 + 768x432",three));
    logs.expect("mixed warm",2,2*kPoolSlots);
    ASSERT_TRUE(timed("1 layer after leaving mixed dimensions",one));
    logs.expect("returned single warm",2,2*kPoolSlots);
    ASSERT_TRUE(run(three,kPoolSlots));
    // Leaving an extent retires its cache. Reentry may create exactly one new
    // six-slot small pool, while the continuously active HD pool stays warm.
    logs.expect("mixed dimensions reentered",3,3*kPoolSlots);
    snapshot(mixed); snapshot(single);
    logs.expect("small pixel snapshots",3,3*kPoolSlots);
}

TEST_F(MixedResolutionInteropTest, SameExtentNv12P010AndOptionalRgba16fDoNotRetainFormatState) {
    CaptureInteropLogs logs;
    logs.expect("fresh format test",0,0);
    const auto nv12 = hardwareSource(device,96,54,DXGI_FORMAT_NV12,{64,100,180},error);
    ASSERT_TRUE(nv12.frame) << error;
    const auto p010 = hardwareSource(device,96,54,DXGI_FORMAT_P010,{192,90,140},error);
    ASSERT_TRUE(p010.frame) << error;
    std::vector<Source> sources{nv12,p010};
    auto rgba = hardwareSource(device,96,54,DXGI_FORMAT_R16G16B16A16_FLOAT,{},error);
    if (rgba.frame) sources.push_back(rgba);
    else std::cout << "[interop formats] optional RGBA16F unavailable: " << error << '\n';
    const auto request = requestFor(sources,96,54);
    ASSERT_TRUE(run(request,kPoolSlots));
    logs.expect("same extent formats prewarmed",1,kPoolSlots);
    ASSERT_TRUE(timed("same extent changing NV12/P010/optional RGBA16F layer order",request,true));
    logs.expect("same extent formats warm",1,kPoolSlots);
    // Read each format in isolation AFTER other formats used the same cache,
    // then check different composition orders with all contributions visible.
    for (const auto& source : sources) snapshot({source});
    snapshot(sources);
    std::reverse(sources.begin(),sources.end()); snapshot(sources);
    logs.expect("format changes and pixel snapshots",1,kPoolSlots);
}

TEST_F(MixedResolutionInteropTest, CancellationDrainsMixedExtentsAndShutdownAllowsFreshPools) {
    CaptureInteropLogs logs;
    logs.expect("fresh cancellation test",0,0);
    const auto large = hardwareSource(device,96,54,DXGI_FORMAT_NV12,{64,100,180},error);
    ASSERT_TRUE(large.frame) << error;
    const auto smallA = hardwareSource(device,64,36,DXGI_FORMAT_P010,{128,160,100},error);
    ASSERT_TRUE(smallA.frame) << error;
    const auto smallB = hardwareSource(device,64,36,DXGI_FORMAT_NV12,{192,90,140},error);
    ASSERT_TRUE(smallB.frame) << error;
    const std::vector<Source> sources{large,smallA,smallB};
    auto request = requestFor(sources,96,54);
    ASSERT_TRUE(run(request,kPoolSlots));
    logs.expect("cancellation prewarm",2,2*kPoolSlots);
    auto lease = renderer->render(request,&error); ASSERT_TRUE(lease) << error;
    const auto image = lease.image();
    const auto beforeCancel = logs.counts();
    renderer->setCancellation([] { return true; });
    EXPECT_FALSE(renderer->render(request,&error));
    EXPECT_NE(error.find("cancelled"),std::string::npos) << error;
    logs.expect("entry cancellation creates no resources",beforeCancel.initializations,beforeCancel.imports);
    renderer->setCancellation({});
    // Permit work to start, then cancel at a later stage/polled wait. Avoid a
    // wall-clock race: callbacks are evaluated synchronously on this worker.
    int cancellationChecks = 0;
    renderer->setCancellation([&] { return ++cancellationChecks >= 6; });
    EXPECT_FALSE(renderer->render(request,&error));
    // ColorGpuPass drains a submitted pass before reporting this error;
    // ColorPipeline preserves its message, including the capital C.
    EXPECT_EQ(error,"Cancelled");
    EXPECT_GE(cancellationChecks,6);
    renderer->setCancellation({});
    snapshot(sources);
    logs.expect("cancelled work drained and recovered",2,2*kPoolSlots);
    const auto beforeShutdown = logs.counts();
    renderer->shutdown();
    EXPECT_EQ(lease.image().image,image.image);
    EXPECT_TRUE(lease.image().ready.valid());
    lease = {}; // Last lease is released while the external GPU still lives.
    logs.expect("shutdown creates no resources",beforeShutdown.initializations,beforeShutdown.imports);
    ASSERT_TRUE(renderer->initialize(gpu->plGpu(),gpu->plVulkan(),device,
                                    D3D11Context::instance().context(),&error)) << error;
    ASSERT_TRUE(run(request,kPoolSlots));
    logs.expect("new renderer session gets fresh bounded pools",beforeShutdown.initializations+2,
                beforeShutdown.imports+2*kPoolSlots);
    snapshot(sources);
    renderer->shutdown();
    logs.expect("second shutdown",beforeShutdown.initializations+2,beforeShutdown.imports+2*kPoolSlots);
    // TearDown also checks the per-test validation delta after all GPU teardown.
}
} // namespace
