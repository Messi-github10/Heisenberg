#include <Compositing/CompositionPlan.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <MultiMedia/Video/Renderer/VideoPresenter.hpp>
#include <MultiMedia/Video/Renderer/RenderEngine.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <Utiles/Logger.hpp>
#include <spdlog/sinks/base_sink.h>
#include <d3d11.h>
#include <array>
#include <cstddef>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
struct GpuFixture {
    std::unique_ptr<GpuContext> gpu;
    std::string error;
    GpuFixture() {
        try {
            auto& context = VulkanContext::instance();
            context.initLoader(); context.createInstance(); context.createDevice();
            D3D11Context::instance().createDevice();
            VulkanResources resources;
            resources.instance = context.vkInstance(); resources.device = context.device();
            resources.physDevice = context.physicalDevice(); resources.graphicsQF = context.graphicsQueueFamily();
            resources.graphicsQueue = context.graphicsQueue(); resources.getProcAddr = context.getInstanceProcAddr();
            resources.enabledFeatures = context.enabledDeviceFeatures();
            resources.enabledExtensions = context.enabledDeviceExtensions();
            gpu = std::make_unique<GpuContext>(resources);
        } catch (const std::exception& e) { error = e.what(); }
    }
    ~GpuFixture() {
        gpu.reset();
        // When validation is requested, errors are test failures rather than
        // stderr noise hidden behind otherwise successful pixel comparisons.
        EXPECT_EQ(VulkanContext::instance().validationErrorCount(), 0u);
    }
};
std::shared_ptr<AVFrame> frame(int format) {
    auto* raw = av_frame_alloc();
    auto result = std::shared_ptr<AVFrame>(raw, [](AVFrame* f) { av_frame_free(&f); });
    raw->format = format; raw->width = 32; raw->height = 24;
    raw->sample_aspect_ratio = {1,1}; raw->time_base = {1,24}; raw->pts = 37;
    raw->color_primaries = AVCOL_PRI_BT709; raw->color_trc = AVCOL_TRC_BT709;
    raw->colorspace = AVCOL_SPC_BT709; raw->color_range = AVCOL_RANGE_MPEG;
    raw->chroma_location = AVCHROMA_LOC_LEFT;
    return result;
}
CompositionRenderRequest request(const std::shared_ptr<AVFrame>& raw) {
    CompositionRenderRequest result; result.source = VideoFrame::wrap(raw,0,Rational{24,1});
    result.settings = TimelineSettings::fromTimelineSettings(32,24,24); result.sourceMode = true;
    return result;
}
void compare(const AVFrame& a, const AVFrame& b, float tolerance) {
    ASSERT_EQ(a.width,b.width); ASSERT_EQ(a.height,b.height);
    for (int y=0;y<a.height;++y) for (int x=0;x<a.width*4;++x) {
        const float av = reinterpret_cast<const float*>(a.data[0]+y*a.linesize[0])[x];
        const float bv = reinterpret_cast<const float*>(b.data[0]+y*b.linesize[0])[x];
        ASSERT_NEAR(av,bv,tolerance) << "row=" << y << " component=" << x;
    }
}
void checkInterop(bool tenBit) {
    GpuFixture fixture;
    if (!fixture.gpu) GTEST_SKIP() << fixture.error;
    auto* device = D3D11Context::instance().device();
    if (!device) GTEST_SKIP() << "No D3D11 hardware device";
    auto software = frame(tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12);
    ASSERT_GE(av_frame_get_buffer(software.get(),32),0);
    const int bytes = tenBit ? 2 : 1;
    std::vector<uint8_t> packed(32*24*3/2*bytes);
    auto store=[&](int index,int value) {
        if (tenBit) { const uint16_t word=static_cast<uint16_t>(value*4 << 6); std::memcpy(packed.data()+index*2,&word,2); }
        else packed[index]=static_cast<uint8_t>(value);
    };
    // Constant chroma excludes reconstruction-kernel differences while distinct
    // Y bands exercise actual range/matrix, and P010 exercises 6 padding bits.
    for (int y=0;y<24;++y) for (int x=0;x<32;++x) store(y*32+x,40+(x/8)*40);
    for (int y=24;y<36;++y) for (int x=0;x<32;x+=2) { store(y*32+x,90); store(y*32+x+1,170); }
    for (int y=0;y<24;++y) std::memcpy(software->data[0]+y*software->linesize[0],packed.data()+y*32*bytes,32*bytes);
    for (int y=0;y<12;++y) std::memcpy(software->data[1]+y*software->linesize[1],packed.data()+(24+y)*32*bytes,32*bytes);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=32; desc.Height=24; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12; desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=packed.data(); data.SysMemPitch=32*bytes;
    ID3D11Texture2D* rawTexture=nullptr;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc,&data,&rawTexture)));
    std::shared_ptr<ID3D11Texture2D> texture(rawTexture,[](auto* p){p->Release();});
    auto hardware=frame(AV_PIX_FMT_D3D11); hardware->data[0]=reinterpret_cast<uint8_t*>(texture.get());
    CompositionRenderer renderer; std::string error;
    ASSERT_TRUE(renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),device,D3D11Context::instance().context(),&error)) << error;
    auto a=renderer.render(request(software),&error); ASSERT_TRUE(a) << error;
    EXPECT_EQ(a.image().contract,filtergraph::kWorkingImageContract);
    auto b=renderer.render(request(hardware),&error); ASSERT_TRUE(b) << error;
    auto ad=renderer.download(a,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(ad) << error;
    auto bd=renderer.download(b,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(bd) << error;
    compare(*ad,*bd,0.006f);
    auto ao=renderer.output(a,{},&error); ASSERT_TRUE(ao) << error;
    auto bo=renderer.output(b,{},&error); ASSERT_TRUE(bo) << error;
    auto aod=renderer.download(ao,DownloadFormat::RgbaFloat,&error);
    auto bod=renderer.download(bo,DownloadFormat::RgbaFloat,&error);
    ASSERT_TRUE(aod) << error; ASSERT_TRUE(bod) << error; compare(*aod,*bod,0.008f);
    EXPECT_EQ(software->pts,37); EXPECT_EQ(hardware->pts,37);
}

class ColorPathLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages() {
        std::lock_guard lock(mutex_); return messages_;
    }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level != spdlog::level::info) return;
        const std::string text(message.payload.data(),message.payload.size());
        if (text.starts_with("CompositionRenderer color path: ")) messages_.push_back(text);
    }
    void flush_() override {}
    std::vector<std::string> messages_;
};
class CaptureColorPaths {
public:
    CaptureColorPaths() {
        logger_=Logger::ModuleLogger("Framework/MultiMedia/Video/Renderer/CompositionRenderer.cpp");
        sinks_=logger_->sinks(); level_=logger_->level();
        logger_->sinks().push_back(sink); logger_->set_level(spdlog::level::info);
    }
    ~CaptureColorPaths() { logger_->sinks()=std::move(sinks_); logger_->set_level(level_); }
    std::shared_ptr<ColorPathLogSink> sink=std::make_shared<ColorPathLogSink>();
private:
    spdlog::logger* logger_=nullptr;
    std::vector<spdlog::sink_ptr> sinks_;
    spdlog::level::level_enum level_=spdlog::level::info;
};

double srgbFromRec1886(double encoded) {
    const double linear = std::pow(std::abs(encoded),2.4);
    const double srgb = linear <= 0.0031308 ? 12.92*linear : 1.055*std::pow(linear,1.0/2.4)-0.055;
    return std::copysign(srgb,encoded);
}
std::array<double,3> independent709(int y, int cb, int cr, bool tenBit, bool fullRange) {
    const double scale = tenBit ? 4.0 : 1.0;
    const double maximum = tenBit ? 1023.0 : 255.0;
    const double luma = fullRange ? y/maximum : (y-16.0*scale)/(219.0*scale);
    // Full YCbCr has a digital center (128/512), not 0.5. Its signed
    // excursion is +/-127/511; limited excursion is +/-112/448.
    const double denominator = fullRange ? maximum-1.0 : 224.0*scale;
    const double u = (cb-128.0*scale)/denominator;
    const double v = (cr-128.0*scale)/denominator;
    constexpr double kr = 0.2126, kb = 0.0722, kg = 1.0-kr-kb;
    return {luma+2.0*(1.0-kr)*v,
        luma-2.0*kb*(1.0-kb)/kg*u-2.0*kr*(1.0-kr)/kg*v,
        luma+2.0*(1.0-kb)*u};
}
// A real readable UNORM attachment on the SAME imported GPU as the renderer.
// The helper borrows the encoded lease only during a synchronous final blit;
// no normal swapchain image is copied or downloaded.
class ReadablePresentationAttachment {
public:
    explicit ReadablePresentationAttachment(pl_gpu gpu) : gpu_(gpu), blitter_(gpu) {}
    ~ReadablePresentationAttachment() {
        pl_gpu_finish(gpu_);
        if (surface_.fbo) pl_tex_destroy(gpu_,&surface_.fbo);
    }
    bool blit(const RenderedFrame& frame, const char* format, std::string& error) {
        if (surface_.fbo) pl_tex_destroy(gpu_,&surface_.fbo);
        const auto image = frame.image();
        pl_tex_params params{};
        params.w = int(image.extent.width); params.h = int(image.extent.height);
        params.format = pl_find_named_fmt(gpu_,format);
        params.renderable = true; params.blit_dst = true; params.host_readable = true;
        if (!params.format || !(params.format->caps & PL_FMT_CAP_RENDERABLE) ||
            !(params.format->caps & PL_FMT_CAP_HOST_READABLE)) { error="Unsupported readable test attachment"; return false; }
        surface_ = {};
        surface_.fbo = pl_tex_create(gpu_,&params);
        if (!surface_.fbo) { error="Cannot create readable test attachment"; return false; }
        surface_.color_repr = pl_color_repr_rgb;
        surface_.color_repr.alpha = PL_ALPHA_INDEPENDENT;
        surface_.color_repr.bits.sample_depth = surface_.color_repr.bits.color_depth = params.format->component_depth[0];
        surface_.color_space = pl_color_space_srgb;
        VkFormat actual = VK_FORMAT_UNDEFINED; VkImageUsageFlags usage = 0;
        if (!pl_vulkan_unwrap(gpu_,surface_.fbo,&actual,&usage) || !(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            error="Test attachment is not a Vulkan TRANSFER_SRC image"; return false;
        }
        pl_vulkan_wrap_params wrap{};
        wrap.image=image.image; wrap.width=params.w; wrap.height=params.h;
        wrap.format=image.vkFormat; wrap.usage=image.usage;
        auto texture = pl_vulkan_wrap(gpu_,&wrap);
        if (!texture) { error="Cannot wrap encoded renderer lease"; return false; }
        pl_vulkan_sem_params semaphoreParams{}; semaphoreParams.type=VK_SEMAPHORE_TYPE_TIMELINE;
        auto semaphore = pl_vulkan_sem_create(gpu_,&semaphoreParams);
        if (!semaphore) { pl_tex_destroy(gpu_,&texture); error="Cannot create test read-completion semaphore"; return false; }
        pl_vulkan_release_params release{};
        release.tex=texture; release.layout=image.layout; release.qf=image.queueFamilyIndex;
        release.semaphore.sem=image.ready.semaphore; release.semaphore.value=image.ready.value;
        pl_vulkan_release_ex(gpu_,&release);
        const bool rendered = blitPresentationFrame(gpu_,blitter_,texture,frame.outputSelectionMetadata(),surface_,&error);
        pl_gpu_flush(gpu_);
        pl_vulkan_hold_params hold{};
        hold.tex=texture; hold.layout=image.layout; hold.qf=image.queueFamilyIndex;
        hold.semaphore.sem=semaphore; hold.semaphore.value=1;
        const bool returned = pl_vulkan_hold_ex(gpu_,&hold);
        pl_gpu_finish(gpu_);
        pl_tex_destroy(gpu_,&texture);
        pl_vulkan_sem_destroy(gpu_,&semaphore);
        if (!rendered || !returned) { if (error.empty()) error="Cannot return encoded lease after test blit"; return false; }
        pixels_.resize(size_t(params.w)*params.h*params.format->texel_size);
        pl_tex_transfer_params transfer{}; transfer.tex=surface_.fbo; transfer.ptr=pixels_.data();
        if (!pl_tex_download(gpu_,&transfer)) { error="Cannot download final presentation attachment"; return false; }
        return true;
    }
    float quantum() const {
        return 1.0f / float((1 << surface_.fbo->params.format->component_depth[0])-1);
    }
    std::array<float,4> pixel(int x, int y) const {
        const auto format=surface_.fbo->params.format;
        uint64_t packed=0;
        std::memcpy(&packed,pixels_.data()+(size_t(y)*surface_.fbo->params.w+x)*format->texel_size,format->texel_size);
        std::array<float,4> value{}; int offset=0;
        for (int c=0; c<format->num_components; ++c) {
            const uint64_t mask=(uint64_t(1) << format->host_bits[c])-1;
            value[format->sample_order[c]]=float((packed >> offset)&mask)/float(mask);
            offset+=format->host_bits[c];
        }
        return value;
    }
private:
    pl_gpu gpu_;
    RenderEngine blitter_;
    pl_swapchain_frame surface_{};
    std::vector<std::byte> pixels_;
};

void checkSdrAppearance(bool tenBit, bool hardware, bool videoPreview = false) {
    GpuFixture fixture; ASSERT_TRUE(fixture.gpu) << fixture.error;
    auto* device = D3D11Context::instance().device();
    if (hardware && !device) GTEST_SKIP() << "No D3D11 hardware device";
    CompositionRenderer renderer; std::string error;
    ASSERT_TRUE(renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),device,D3D11Context::instance().context(),&error)) << error;
    for (const auto range : {AVCOL_RANGE_MPEG,AVCOL_RANGE_JPEG,AVCOL_RANGE_UNSPECIFIED}) {
        const bool full = range == AVCOL_RANGE_JPEG;
        for (const int chroma : {0,1,2}) {
            SCOPED_TRACE(testing::Message() << "tenBit=" << tenBit << " hardware=" << hardware << " range=" << int(range) << " chroma=" << chroma);
            const int scale = tenBit ? 4 : 1, bytes = tenBit ? 2 : 1;
            auto software = frame(tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12);
            software->color_range = range;
            ASSERT_GE(av_frame_get_buffer(software.get(),32),0);
            const std::array<int,4> base = chroma ? std::array<int,4>{64,96,128,160}
                : full ? std::array<int,4>{0,8,128,255} : std::array<int,4>{16,20,128,235};
            std::array<int,4> ys{};
            for (size_t i=0; i<ys.size(); ++i)
                ys[i] = tenBit && full && base[i] == 255 ? 1023
                    : base[i]*scale+(tenBit && base[i] != 0 && base[i] != 16 && base[i] != 235 ? 1 : 0);
            const int cb = (chroma == 1 ? 112 : chroma == 2 ? 152 : 128)*scale+(tenBit && chroma ? 1 : 0);
            const int cr = (chroma == 1 ? 148 : chroma == 2 ? 110 : 128)*scale+(tenBit && chroma ? 2 : 0);
            std::vector<uint8_t> packed(32*24*3/2*bytes);
            auto store = [&](int index,int code) {
                if (tenBit) { const uint16_t word = static_cast<uint16_t>(code << 6); std::memcpy(packed.data()+index*2,&word,2); }
                else packed[index] = static_cast<uint8_t>(code);
            };
            for (int y=0; y<24; ++y) for (int x=0; x<32; ++x) store(y*32+x,ys[x/8]);
            for (int y=24; y<36; ++y) for (int x=0; x<32; x+=2) { store(y*32+x,cb); store(y*32+x+1,cr); }
            for (int y=0; y<24; ++y) std::memcpy(software->data[0]+y*software->linesize[0],packed.data()+y*32*bytes,32*bytes);
            for (int y=0; y<12; ++y) std::memcpy(software->data[1]+y*software->linesize[1],packed.data()+(24+y)*32*bytes,32*bytes);
            auto source = software;
            std::shared_ptr<ID3D11Texture2D> texture;
            if (hardware) {
                D3D11_TEXTURE2D_DESC desc{}; desc.Width=32; desc.Height=24; desc.MipLevels=1; desc.ArraySize=1;
                desc.Format=tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12; desc.SampleDesc.Count=1;
                desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=packed.data(); data.SysMemPitch=32*bytes;
                ID3D11Texture2D* rawTexture=nullptr;
                ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc,&data,&rawTexture)));
                texture = std::shared_ptr<ID3D11Texture2D>(rawTexture,[](auto* p) { p->Release(); });
                source=frame(AV_PIX_FMT_D3D11); source->color_range=range;
                source->data[0]=reinterpret_cast<uint8_t*>(texture.get());
            }
            // Test the no-plan policy2 path and the same source under an actual
            // plan containing explicitly neutral enabled grade parameters.
            CompositionPlan plan; plan.settings=TimelineSettings::fromTimelineSettings(32,24,24);
            CompositionClip clip; clip.id="neutral"; clip.mediaRef="source"; clip.durationFrames=24;
            clip.visualSnapshot.effects=JsonValue::parse(R"([{"type":"video.basicGrade","params":{"exposure":{"value":0},"contrast":{"value":1},"saturation":{"value":1}}}])");
            plan.clips.push_back(clip);
            std::shared_ptr<AVFrame> ungraded;
            for (const bool identityGrade : {false,true}) {
                auto req=request(source); req.sourceId="sdr-range-fixture";
                if (identityGrade) { req.plan=&plan; req.clipId=clip.id; }
                std::vector<std::string> diagnostics; req.diagnostics=&diagnostics;
                auto working=renderer.render(req,&error); ASSERT_TRUE(working) << error;
                EXPECT_TRUE(working.outputSelectionMetadata().display.empty());
                if (videoPreview) {
                    const auto before=renderer.download(working,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(before) << error;
                    const ColorOutputRequest previewRequest{color::kPreviewDisplay,color::kColorimetricView,{},ColorOutputIntent::SdrVideoPreview};
                    auto encoded=renderer.output(working,previewRequest,&error); ASSERT_TRUE(encoded) << error;
                    EXPECT_EQ(encoded.outputSelectionMetadata().display,color::kExportDisplay);
                    EXPECT_EQ(encoded.outputSelectionMetadata().view,color::kColorimetricView);
                    EXPECT_TRUE(encoded.outputSelectionMetadata().colorSpace.empty());
                    EXPECT_EQ(encoded.outputSelectionMetadata().intent,ColorOutputIntent::SdrVideoPreview);
                    auto codes=renderer.download(encoded,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(codes) << error;
                    // Export still has the SAME actual Rec1886 encoding but no
                    // runtime preview intent; its numerical OCIO roundtrip is
                    // identity before a presenter ever sees the image.
                    const ColorOutputRequest exportRequest{color::kExportDisplay,color::kColorimetricView,{}};
                    EXPECT_EQ(exportRequest.intent,ColorOutputIntent::Colorimetric);
                    auto exported=renderer.output(working,exportRequest,&error); ASSERT_TRUE(exported) << error;
                    EXPECT_EQ(exported.outputSelectionMetadata().display,color::kExportDisplay);
                    EXPECT_EQ(exported.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
                    auto exportCodes=renderer.download(exported,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(exportCodes) << error;
                    compare(*codes,*exportCodes,0.00001f);
                    for (int band=0; band<4; ++band) {
                        const auto expected=independent709(ys[band],cb,cr,tenBit,full);
                        const auto* actual=reinterpret_cast<const float*>(codes->data[0]+12*codes->linesize[0])+(band*8+4)*4;
                        for (int c=0; c<3; ++c) {
                            const double budget=!tenBit && chroma ? 0.5/255.0 : 0.0015;
                            EXPECT_NEAR(actual[c],expected[c],std::abs(expected[c])<0.005 ? 0.003 : budget)
                                << "pre-present band=" << band << " channel=" << c << " neutralGrade=" << identityGrade;
                        }
                    }
                    if (!identityGrade) ungraded=codes;
                    else { ASSERT_TRUE(ungraded); compare(*codes,*ungraded,0.00001f); }
                    ReadablePresentationAttachment attachment(fixture.gpu->plGpu());
                    for (const char* format : {"rgba8","rgb10a2"}) {
                        SCOPED_TRACE(format);
                        ASSERT_TRUE(attachment.blit(encoded,format,error)) << error;
                        for (int band=0; band<4; ++band) {
                            const auto expected=independent709(ys[band],cb,cr,tenBit,full);
                            const auto actual=attachment.pixel(band*8+4,12);
                            for (int c=0; c<3; ++c) {
                                const double unpackBudget=!tenBit && chroma ? 0.5/255.0 : 0.0015;
                                EXPECT_NEAR(actual[c],std::clamp(expected[c],0.0,1.0),unpackBudget+2.0*attachment.quantum())
                                    << "final band=" << band << " channel=" << c << " neutralGrade=" << identityGrade;
                            }
                            EXPECT_FLOAT_EQ(actual[3],1.0f);
                        }
                    }
                    const auto after=renderer.download(working,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(after) << error;
                    compare(*before,*after,0.0f);
                    EXPECT_FALSE(working.encoded());
                    EXPECT_EQ(working.image().contract,filtergraph::kWorkingImageContract);
                    EXPECT_EQ(working.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
                    continue;
                }
                for (const bool preview : {false,true}) {
                    ColorOutputRequest output;
                    if (!preview) { output.display=color::kExportDisplay; output.view=color::kColorimetricView; }
                    auto encoded=renderer.output(working,output,&error); ASSERT_TRUE(encoded) << error;
                    EXPECT_EQ(encoded.outputSelectionMetadata().display,preview ? color::kPreviewDisplay : color::kExportDisplay);
                    EXPECT_EQ(encoded.outputSelectionMetadata().view,color::kColorimetricView);
                    EXPECT_TRUE(encoded.outputSelectionMetadata().colorSpace.empty());
                    auto pixels=renderer.download(encoded,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(pixels) << error;
                    for (int band=0; band<4; ++band) {
                        const auto expected=independent709(ys[band],cb,cr,tenBit,full);
                        const auto* actual=reinterpret_cast<const float*>(pixels->data[0]+12*pixels->linesize[0])+(band*8+4)*4;
                        for (int c=0; c<3; ++c) {
                            const double reference=preview ? srgbFromRec1886(expected[c]) : expected[c];
                            // Keep grayscale and P010 at the tighter FP16 budget.
                            // NV12 chroma unpack adds sub-code precision loss:
                            // allow at most half ONE 8-bit encoded RGB code, not
                            // a percentage of brightness or a multi-code shift.
                            // The independent sRGB curve propagates that same
                            // encoded-Rec1886 interval for preview, including toe.
                            const double codeBudget=!tenBit && chroma ? 0.5/255.0 : 0.0015;
                            const double tolerance=preview ? std::max(
                                std::abs(srgbFromRec1886(expected[c]-codeBudget)-reference),
                                std::abs(srgbFromRec1886(expected[c]+codeBudget)-reference))
                                : std::abs(reference)<0.005 ? 0.003 : codeBudget;
                            EXPECT_NEAR(actual[c],reference,tolerance) << "band=" << band << " channel=" << c << " neutralGrade=" << identityGrade << " preview=" << preview;
                        }
                        EXPECT_NEAR(actual[3],1.0f,0.001f);
                    }
                    if (!preview) {
                        if (!identityGrade) ungraded=pixels;
                        else { ASSERT_TRUE(ungraded); compare(*pixels,*ungraded,0.00001f); }
                    }
                }
            }
        }
    }
}
}

TEST(SdrAppearanceTest, Nv12SoftwareRangeAndNeutralGradeMatchIndependentReference) { checkSdrAppearance(false,false); }
TEST(SdrAppearanceTest, P010SoftwareRangeAndNeutralGradeMatchIndependentReference) { checkSdrAppearance(true,false); }
TEST(SdrAppearanceTest, Nv12D3DRangeAndNeutralGradeMatchIndependentReference) { checkSdrAppearance(false,true); }
TEST(SdrAppearanceTest, P010D3DRangeAndNeutralGradeMatchIndependentReference) { checkSdrAppearance(true,true); }

TEST(SdrVideoPreviewTest, Nv12SoftwareUnpackedCodesSurviveOutputAndActualFinalBlit) { checkSdrAppearance(false,false,true); }
TEST(SdrVideoPreviewTest, P010SoftwareUnpackedCodesSurviveOutputAndActualFinalBlit) { checkSdrAppearance(true,false,true); }
TEST(SdrVideoPreviewTest, Nv12D3DUnpackedCodesSurviveOutputAndActualFinalBlit) { checkSdrAppearance(false,true,true); }
TEST(SdrVideoPreviewTest, P010D3DUnpackedCodesSurviveOutputAndActualFinalBlit) { checkSdrAppearance(true,true,true); }

TEST(SdrVideoPreviewTest, InputInterpretationAndOutputWhitelistDoNotLeakAcrossWorkingLeases) {
    GpuFixture fixture; ASSERT_TRUE(fixture.gpu) << fixture.error;
    CompositionRenderer renderer; std::string error;
    ASSERT_TRUE(renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    auto source=frame(AV_PIX_FMT_RGBAF32);
    source->color_range=AVCOL_RANGE_JPEG; source->colorspace=AVCOL_SPC_RGB;
    ASSERT_GE(av_frame_get_buffer(source.get(),32),0);
    constexpr std::array<std::array<float,3>,4> values{{
        {0.01f,0.01f,0.01f}, {0.18f,0.18f,0.18f}, {0.5f,0.5f,0.5f}, {0.18f,0.5f,0.75f}}};
    for (int y=0; y<24; ++y) {
        auto* row=reinterpret_cast<float*>(source->data[0]+y*source->linesize[0]);
        for (int x=0; x<32; ++x) {
            for (int c=0; c<3; ++c) row[x*4+c]=values[x/8][c];
            row[x*4+3]=1.0f;
        }
    }
    auto req=request(source);
    auto ordinary=renderer.render(req,&error); ASSERT_TRUE(ordinary) << error;
    const auto ordinaryWorking=renderer.download(ordinary,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(ordinaryWorking) << error;
    const ColorOutputRequest preview{color::kPreviewDisplay,color::kColorimetricView,{},ColorOutputIntent::SdrVideoPreview};
    // Render other interpretations before outputting the original lease. The
    // eligibility belongs to that lease, never to renderer's latest source.
    for (const char* space : {"sRGB - Display","sRGB Encoded Rec.709 (sRGB)","Linear Rec.709 (sRGB)","ACEScg"}) {
        SCOPED_TRACE(space);
        req.inputColorSpace=space;
        auto working=renderer.render(req,&error); ASSERT_TRUE(working) << error;
        const auto baseline=renderer.output(working,{},&error); ASSERT_TRUE(baseline) << error;
        const auto output=renderer.output(working,preview,&error); ASSERT_TRUE(output) << error;
        EXPECT_EQ(output.outputSelectionMetadata().display,color::kPreviewDisplay);
        EXPECT_EQ(output.outputSelectionMetadata().view,color::kColorimetricView);
        EXPECT_EQ(output.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
        const auto baselinePixels=renderer.download(baseline,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(baselinePixels) << error;
        const auto pixels=renderer.download(output,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(pixels) << error;
        compare(*baselinePixels,*pixels,0.0f);
        if (std::string(space)=="sRGB - Display" || std::string(space)=="sRGB Encoded Rec.709 (sRGB)") {
            ReadablePresentationAttachment attachment(fixture.gpu->plGpu());
            for (const char* format : {"rgba8","rgb10a2"}) {
                ASSERT_TRUE(attachment.blit(output,format,error)) << error;
                for (int band=0; band<4; ++band) for (int c=0; c<3; ++c)
                    EXPECT_NEAR(attachment.pixel(band*8+4,12)[c],values[band][c],0.0015f+2.0f*attachment.quantum());
            }
        }
    }
    auto eligible=renderer.output(ordinary,preview,&error); ASSERT_TRUE(eligible) << error;
    EXPECT_EQ(eligible.outputSelectionMetadata().display,color::kExportDisplay);
    EXPECT_EQ(eligible.outputSelectionMetadata().intent,ColorOutputIntent::SdrVideoPreview);
    const auto eligiblePixels=renderer.download(eligible,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(eligiblePixels) << error;
    for (int band=0; band<4; ++band) {
        const auto* pixel=reinterpret_cast<const float*>(eligiblePixels->data[0]+12*eligiblePixels->linesize[0])+(band*8+4)*4;
        for (int c=0; c<3; ++c) EXPECT_NEAR(pixel[c],values[band][c],0.0015f);
    }
    // ACES view and direct-space requests must preserve their existing OCIO
    // result and normalize away preview intent, including export Rec1886.
    for (auto selection : std::array<ColorOutputRequest,6>{{
             {color::kPreviewDisplay,color::kAcesOutputView,{}},
             {color::kExportDisplay,color::kColorimetricView,{}},
             {{}, {}, color::kExportDisplay},
             {{}, {}, "sRGB - Display"},
             {{}, {}, "Linear Rec.709 (sRGB)"},
             {{}, {}, "ACEScg"}}}) {
        const auto baseline=renderer.output(ordinary,selection,&error); ASSERT_TRUE(baseline) << error;
        selection.intent=ColorOutputIntent::SdrVideoPreview;
        const auto output=renderer.output(ordinary,selection,&error); ASSERT_TRUE(output) << error;
        EXPECT_EQ(output.outputSelectionMetadata().display,baseline.outputSelectionMetadata().display);
        EXPECT_EQ(output.outputSelectionMetadata().view,baseline.outputSelectionMetadata().view);
        EXPECT_EQ(output.outputSelectionMetadata().colorSpace,baseline.outputSelectionMetadata().colorSpace);
        EXPECT_EQ(output.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
        const auto baselinePixels=renderer.download(baseline,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(baselinePixels) << error;
        const auto pixels=renderer.download(output,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(pixels) << error;
        compare(*baselinePixels,*pixels,0.0f);
    }
    CompositionPlan legacy; legacy.settings=req.settings; legacy.colorSettings=legacyColorSettings();
    req.plan=&legacy; req.inputColorSpace=color::kExportDisplay;
    auto legacyWorking=renderer.render(req,&error); ASSERT_TRUE(legacyWorking) << error;
    auto legacyOutput=renderer.output(legacyWorking,preview,&error); ASSERT_TRUE(legacyOutput) << error;
    EXPECT_EQ(legacyOutput.outputSelectionMetadata().display,color::kPreviewDisplay);
    EXPECT_EQ(legacyOutput.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
    const auto ordinaryAfter=renderer.download(ordinary,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(ordinaryAfter) << error;
    compare(*ordinaryWorking,*ordinaryAfter,0.0f);
    EXPECT_EQ(ordinary.image().contract,filtergraph::kWorkingImageContract);
    EXPECT_FALSE(ordinary.encoded());
    EXPECT_TRUE(ordinary.outputSelectionMetadata().display.empty());
    EXPECT_EQ(ordinary.outputSelectionMetadata().intent,ColorOutputIntent::Colorimetric);
}

TEST(SdrAppearanceTest, PathInfoDeduplicatesActualMetadataPolicySourceAndOutputNotFrames) {
    GpuFixture fixture; ASSERT_TRUE(fixture.gpu) << fixture.error;
    CompositionRenderer renderer; std::string error;
    ASSERT_TRUE(renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    auto source=frame(AV_PIX_FMT_NV12); source->color_range=AVCOL_RANGE_UNSPECIFIED;
    source->color_primaries=AVCOL_PRI_UNSPECIFIED; source->color_trc=AVCOL_TRC_UNSPECIFIED;
    ASSERT_GE(av_frame_get_buffer(source.get(),32),0);
    for (int y=0; y<24; ++y) std::memset(source->data[0]+y*source->linesize[0],128,32);
    for (int y=0; y<12; ++y) std::memset(source->data[1]+y*source->linesize[1],128,32);
    CaptureColorPaths capture;
    auto req=request(source); req.sourceId="path-source-a";
    std::vector<std::string> diagnostics; req.diagnostics=&diagnostics;
    auto renderOutput=[&](const ColorOutputRequest& output=ColorOutputRequest{}) {
        diagnostics.clear(); auto working=renderer.render(req,&error);
        if (!working) return RenderedFrame{};
        return renderer.output(working,output,&error);
    };
    for (int i=0; i<4; ++i) { req.targetFrame=i; ASSERT_TRUE(renderOutput()) << error; }
    auto messages=capture.sink->messages(); ASSERT_EQ(messages.size(),1u);
    for (const char* text : {"source='path-source-a'", "pri=2", "trc=2", "matrix=1", "range=0",
             "format=", "chroma=", "fallback limited", "interpretedInput='Rec.1886 Rec.709 - Display'",
             "selectionOrigin=fallback", "policy=2", "outputDisplay='sRGB - Display'", "outputView='Un-tone-mapped'"})
        EXPECT_NE(messages[0].find(text),std::string::npos) << text;
    ASSERT_FALSE(diagnostics.empty());
    for (const auto& diagnostic : diagnostics) EXPECT_EQ(diagnostic.find("color path"),std::string::npos);
    ColorOutputRequest exported; exported.display=color::kExportDisplay;
    ASSERT_TRUE(renderOutput(exported)) << error;
    EXPECT_EQ(capture.sink->messages().size(),2u);
    ASSERT_TRUE(renderOutput()) << error; // existing signature, not just last signature
    EXPECT_EQ(capture.sink->messages().size(),2u);
    req.sourceId="path-source-b"; ASSERT_TRUE(renderOutput()) << error;
    EXPECT_EQ(capture.sink->messages().size(),3u);
    source->color_range=AVCOL_RANGE_JPEG; req.source=VideoFrame::wrap(source,0,Rational{24,1});
    ASSERT_TRUE(renderOutput()) << error; EXPECT_EQ(capture.sink->messages().size(),4u);
    source->chroma_location=AVCHROMA_LOC_CENTER; req.source=VideoFrame::wrap(source,0,Rational{24,1});
    ASSERT_TRUE(renderOutput()) << error; EXPECT_EQ(capture.sink->messages().size(),5u);
    CompositionPlan legacy; legacy.settings=req.settings; legacy.colorSettings=legacyColorSettings(); req.plan=&legacy;
    ASSERT_TRUE(renderOutput()) << error; EXPECT_EQ(capture.sink->messages().size(),6u);
    EXPECT_NE(capture.sink->messages().back().find("interpretedInput='Camera Rec.709'"),std::string::npos);
    req.inputColorSpace="Camera Rec.709";
    ASSERT_TRUE(renderOutput()) << error; EXPECT_EQ(capture.sink->messages().size(),7u);
    EXPECT_NE(capture.sink->messages().back().find("selectionOrigin=override"),std::string::npos);
    ColorOutputRequest aces; aces.view=color::kAcesOutputView;
    auto explicitAces=renderOutput(aces); ASSERT_TRUE(explicitAces) << error;
    EXPECT_EQ(explicitAces.outputSelectionMetadata().view,color::kAcesOutputView);
    EXPECT_EQ(capture.sink->messages().size(),8u);
    ColorOutputRequest direct; direct.colorSpace=color::kExportDisplay;
    auto directFrame=renderOutput(direct); ASSERT_TRUE(directFrame) << error;
    EXPECT_EQ(directFrame.outputSelectionMetadata().colorSpace,color::kExportDisplay);
    EXPECT_TRUE(directFrame.outputSelectionMetadata().display.empty());
    EXPECT_TRUE(directFrame.outputSelectionMetadata().view.empty());
    EXPECT_EQ(capture.sink->messages().size(),9u);
    // LRU capacity: the last repeated source remains hot, the oldest falls out.
    for (int i=0; i<65; ++i) {
        req.sourceId="bounded-source-"+std::to_string(i);
        ASSERT_TRUE(renderOutput()) << error;
    }
    const auto count=capture.sink->messages().size();
    ASSERT_TRUE(renderOutput()) << error; EXPECT_EQ(capture.sink->messages().size(),count);
    req.sourceId="bounded-source-0"; ASSERT_TRUE(renderOutput()) << error;
    EXPECT_EQ(capture.sink->messages().size(),count+1);
}

TEST(CompositionRendererInteropTest, ImportedDeviceReportsOnlyActuallyEnabledFeaturesAndExtensions) {
    GpuFixture fixture;
    ASSERT_TRUE(fixture.gpu) << fixture.error;
    auto& context = VulkanContext::instance();
    const auto* enabled = context.enabledDeviceFeatures();
    ASSERT_NE(enabled, nullptr);
    const auto* vk11 = static_cast<const VkPhysicalDeviceVulkan11Features*>(enabled->pNext);
    ASSERT_NE(vk11, nullptr);
    const auto* vk12 = static_cast<const VkPhysicalDeviceVulkan12Features*>(vk11->pNext);
    ASSERT_NE(vk12, nullptr);
    EXPECT_EQ(vk12->timelineSemaphore, VK_TRUE);
    EXPECT_EQ(vk12->hostQueryReset, VK_TRUE);
    EXPECT_EQ(enabled->features.shaderStorageImageExtendedFormats, VK_TRUE);
    const auto* sync2 = static_cast<const VkPhysicalDeviceSynchronization2Features*>(vk12->pNext);
    ASSERT_NE(sync2, nullptr);
    EXPECT_EQ(sync2->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
    EXPECT_EQ(sync2->synchronization2, VK_TRUE);
    // Only requested features/extensions may leak through import.
    const auto imported = fixture.gpu->plVulkan();
    EXPECT_LE(imported->api_version, VK_API_VERSION_1_2);
    auto actual = context.enabledDeviceExtensions();
    for (int i = 0; i < imported->num_extensions; ++i)
        EXPECT_TRUE(std::any_of(actual.begin(), actual.end(), [&](const char* extension) {
            return std::strcmp(extension, imported->extensions[i]) == 0;
        })) << imported->extensions[i];
    ASSERT_NE(imported->features, nullptr);
    EXPECT_EQ(imported->features->features.shaderInt64, enabled->features.shaderInt64);
}

TEST(CompositionRendererInteropTest, Nv12SoftwareAndD3DUnpackShareTheSameOCIOPipeline) { checkInterop(false); }
TEST(CompositionRendererInteropTest, P010SoftwareAndD3DUnpackShareTheSameOCIOPipeline) { checkInterop(true); }

TEST(CompositionRendererInteropTest, OwnedLeasesSurviveNextRenderAndRendererShutdown) {
    GpuFixture fixture; if (!fixture.gpu) GTEST_SKIP() << fixture.error;
    CompositionRenderer renderer; std::string error;
    ASSERT_TRUE(renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),nullptr,nullptr,&error)) << error;
    CompositionRenderRequest blank; blank.blank=true; blank.settings=TimelineSettings::fromTimelineSettings(32,24,24);
    blank.targetFrame=17; auto first=renderer.render(blank,&error); ASSERT_TRUE(first) << error;
    const auto firstImage=first.image(); auto lease=first;
    blank.targetFrame=18; auto second=renderer.render(blank,&error); ASSERT_TRUE(second) << error;
    EXPECT_NE(first.image().image,second.image().image);
    auto readback=renderer.download(lease,DownloadFormat::RgbaFloat,&error); ASSERT_TRUE(readback) << error;
    EXPECT_EQ(readback->pts,17);
    EXPECT_FLOAT_EQ(reinterpret_cast<float*>(readback->data[0])[3],1.0f);
    renderer.setCancellation([]{return true;});
    EXPECT_FALSE(renderer.render(blank,&error)); EXPECT_NE(error.find("cancelled"),std::string::npos);
    renderer.setCancellation({});
    renderer.shutdown();
    EXPECT_EQ(lease.image().image,firstImage.image);
    EXPECT_TRUE(lease.image().ready.valid());
    first={}; lease={}; second={};
}

TEST(CompositionRendererInteropTest, IndependentWorkersImportAndSubmitOneSharedQueue) {
    auto worker=[] {
        GpuFixture fixture;
        if (!fixture.gpu) return fixture.error;
        CompositionRenderer renderer; std::string error;
        if (!renderer.initialize(fixture.gpu->plGpu(),fixture.gpu->plVulkan(),nullptr,nullptr,&error)) return error;
        CompositionRenderRequest request; request.blank=true; request.settings=TimelineSettings::fromTimelineSettings(32,24,24);
        for (int i=0;i<4;++i) {
            auto frame=renderer.render(request,&error); if (!frame) return error;
            auto output=renderer.output(frame,{},&error); if (!output) return error;
            auto pixels=renderer.download(output,DownloadFormat::RgbaFloat,&error); if (!pixels) return error;
        }
        return std::string{};
    };
    auto a=std::async(std::launch::async,worker); auto b=std::async(std::launch::async,worker);
    EXPECT_EQ(a.get(),""); EXPECT_EQ(b.get(),"");
}
