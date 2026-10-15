#include <volk.h>
#include <MultiMedia/Video/Renderer/VideoPresenter.hpp>
#include <MultiMedia/Video/Renderer/RenderEngine.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace heisenberg::renderer;

// Analytic IEC sRGB function, independent of OCIO/libplacebo CPU helpers.
float encodeSrgb(float linear) {
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

class VideoPresenterPixelTest : public testing::Test {
protected:
    std::unique_ptr<GpuContext> imported;
    std::unique_ptr<RenderEngine> blitter;
    pl_gpu gpu = nullptr;
    std::vector<pl_tex> textures;
    uint64_t validationBaseline = 0;
    void SetUp() override {
        auto& shared = VulkanContext::instance();
        shared.initLoader();
        shared.createInstance();
        shared.createDevice();
        validationBaseline = shared.validationErrorCount();
        VulkanResources resources;
        resources.instance = shared.vkInstance();
        resources.physDevice = shared.physicalDevice();
        resources.device = shared.device();
        resources.graphicsQF = shared.graphicsQueueFamily();
        resources.graphicsQueue = shared.graphicsQueue();
        resources.getProcAddr = shared.getInstanceProcAddr();
        resources.enabledFeatures = shared.enabledDeviceFeatures();
        resources.enabledExtensions = shared.enabledDeviceExtensions();
        imported = std::make_unique<GpuContext>(resources);
        gpu = imported->plGpu();
        blitter = std::make_unique<RenderEngine>(gpu);
    }
    void TearDown() override {
        if (gpu) {
            pl_gpu_finish(gpu);
            blitter.reset();
            for (auto texture : textures) pl_tex_destroy(gpu, &texture);
            imported.reset();
        }
        auto& shared = VulkanContext::instance();
        EXPECT_EQ(shared.validationErrorCount(), validationBaseline);
        EXPECT_EQ(volkGetLoadedInstance(), static_cast<VkInstance>(shared.vkInstance()));
        EXPECT_EQ(volkGetLoadedDevice(), static_cast<VkDevice>(shared.device()));
        EXPECT_NO_THROW(shared.createDevice());
    }
    pl_tex source(const std::vector<float>& pixels, int width, int height) {
        pl_tex_params params{};
        params.w = width; params.h = height;
        params.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
        params.sampleable = true; params.initial_data = pixels.data();
        const auto result = pl_tex_create(gpu, &params);
        if (result) textures.push_back(result);
        return result;
    }
    pl_swapchain_frame target(const char* format, int width, int height, bool flipped = false) {
        pl_tex_params params{};
        params.w = width; params.h = height;
        params.format = pl_find_named_fmt(gpu, format);
        params.renderable = true; params.blit_dst = true; params.host_readable = true;
        if (!params.format || !(params.format->caps & PL_FMT_CAP_RENDERABLE) ||
            !(params.format->caps & PL_FMT_CAP_HOST_READABLE)) return {};
        const auto result = pl_tex_create(gpu, &params);
        if (result) textures.push_back(result);
        pl_swapchain_frame surface{};
        surface.fbo = result;
        surface.flipped = flipped;
        surface.color_repr = pl_color_repr_rgb;
        surface.color_repr.alpha = PL_ALPHA_INDEPENDENT;
        surface.color_repr.bits.sample_depth = surface.color_repr.bits.color_depth = params.format->component_depth[0];
        surface.color_space = pl_color_space_srgb;
        return surface;
    }
    std::vector<std::byte> download(pl_tex texture) {
        std::vector<std::byte> pixels(size_t(texture->params.w) * texture->params.h * texture->params.format->texel_size);
        VkFormat actual = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        EXPECT_NE(pl_vulkan_unwrap(gpu, texture, &actual, &usage), VK_NULL_HANDLE);
        // Read back only an attachment CREATED with host_readable/TRANSFER_SRC.
        // No copy/readback is attempted from a normal swapchain image.
        EXPECT_TRUE(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        EXPECT_TRUE(texture->params.host_readable);
        pl_tex_transfer_params transfer{}; transfer.tex = texture; transfer.ptr = pixels.data();
        EXPECT_TRUE(pl_tex_download(gpu, &transfer));
        std::cout << "Presenter pixel readback: VkFormat=" << int(actual) << " plformat=" << texture->params.format->name
                  << " " << texture->params.w << "x" << texture->params.h << '\n';
        return pixels;
    }
    std::array<float,4> pixel(const std::vector<std::byte>& pixels, pl_tex texture, int x, int y) {
        const auto fmt = texture->params.format;
        uint64_t packed = 0;
        std::memcpy(&packed, pixels.data() + (size_t(y) * texture->params.w + x) * fmt->texel_size, fmt->texel_size);
        std::array<float,4> rgba{};
        int offset = 0;
        for (int i = 0; i < fmt->num_components; ++i) {
            const uint64_t mask = (uint64_t(1) << fmt->host_bits[i]) - 1;
            rgba[fmt->sample_order[i]] = float((packed >> offset) & mask) / float(mask);
            offset += fmt->host_bits[i];
        }
        return rgba;
    }
};

TEST_F(VideoPresenterPixelTest, EncodedSrgbGraySurvivesActualFinalBlitWithoutDoubleTransfer) {
    constexpr std::array levels{0.0f, 0.003f, 0.018f, 0.09f, 0.18f, 0.5f, 0.75f, 1.0f};
    std::vector<float> pixels;
    for (float level : levels) pixels.insert(pixels.end(), {level, level, level, 1.0f});
    auto input = source(pixels, int(levels.size()), 1); ASSERT_NE(input, nullptr);
    const ColorOutputRequest selection{"sRGB - Display", "Un-tone-mapped", {}};
    for (const char* name : {"rgba8", "bgra8", "rgb10a2", "bgr10a2"}) {
        SCOPED_TRACE(name);
        auto surface = target(name, int(levels.size()), 1);
        ASSERT_NE(surface.fbo, nullptr);
        std::string error;
        ASSERT_TRUE(blitPresentationFrame(gpu, *blitter, input, selection, surface, &error)) << error;
        auto values = download(surface.fbo);
        const float quantum = 1.0f / ((1 << surface.fbo->params.format->component_depth[0]) - 1);
        for (size_t x = 0; x < levels.size(); ++x) {
            const auto actual = pixel(values, surface.fbo, int(x), 0);
            for (int c = 0; c < 3; ++c) EXPECT_NEAR(actual[c], levels[x], 2.0f * quantum) << "gray " << x;
            EXPECT_FLOAT_EQ(actual[3], 1.0f);
        }
        // A second sRGB encoding would send 0.18 to ~0.461, decoding twice
        // would send it to ~0.027. The final attachment must contain ~0.18.
        EXPECT_GT(std::abs(pixel(values, surface.fbo, 4, 0)[0] - encodeSrgb(0.18f)), 0.2f);
    }
}

TEST_F(VideoPresenterPixelTest, Rec1886OutputIsAdaptedToSrgbExactlyOnce) {
    constexpr std::array levels{0.0f, 0.01f, 0.05f, 0.18f, 0.5f, 0.75f, 1.0f};
    std::vector<float> pixels;
    for (float level : levels) pixels.insert(pixels.end(), {level, level, level, 1.0f});
    auto input = source(pixels, int(levels.size()), 1); ASSERT_NE(input, nullptr);
    auto surface = target("rgba8", int(levels.size()), 1); ASSERT_NE(surface.fbo, nullptr);
    std::string error;
    ASSERT_TRUE(blitPresentationFrame(gpu, *blitter, input,
        {"Rec.1886 Rec.709 - Display", "Un-tone-mapped", {}}, surface, &error)) << error;
    auto values = download(surface.fbo);
    for (size_t x = 0; x < levels.size(); ++x) {
        const float expected = encodeSrgb(std::pow(levels[x], 2.4f));
        for (int c = 0; c < 3; ++c) EXPECT_NEAR(pixel(values, surface.fbo, int(x), 0)[c], expected, 2.0f / 255.0f) << "gray " << x;
    }
}

TEST_F(VideoPresenterPixelTest, ExplicitRec1886VideoPreviewPreservesGrayAndColorCodesOnEightAndTenBitUnorm) {
    constexpr std::array levels{0.0f, 0.01f, 0.05f, 0.18f, 0.5f, 0.75f, 1.0f};
    std::vector<std::array<float,4>> reference;
    for (float level : levels) reference.push_back({level, level, level, 1.0f});
    // Asymmetric RGB patches catch channel swaps and an accidental transfer
    // change that a white/primary-only color bar could not detect.
    for (const auto& patch : std::array<std::array<float,4>,8>{{
             {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1},
             {0,1,1,1}, {1,0,1,1}, {0.18f,0.5f,0.75f,1}, {0.75f,0.05f,0.01f,1}}})
        reference.push_back(patch);
    std::vector<float> pixels;
    for (const auto& patch : reference) pixels.insert(pixels.end(),patch.begin(),patch.end());
    auto input = source(pixels,int(reference.size()),1); ASSERT_NE(input,nullptr);
    const ColorOutputRequest selection{"Rec.1886 Rec.709 - Display", "Un-tone-mapped", {},
        ColorOutputIntent::SdrVideoPreview};
    for (const char* name : {"rgba8", "bgra8", "rgb10a2", "bgr10a2"}) {
        SCOPED_TRACE(name);
        auto surface = target(name,int(reference.size()),1); ASSERT_NE(surface.fbo,nullptr);
        std::string error;
        ASSERT_TRUE(blitPresentationFrame(gpu,*blitter,input,selection,surface,&error)) << error;
        const auto values = download(surface.fbo);
        const float quantum = 1.0f / ((1 << surface.fbo->params.format->component_depth[0]) - 1);
        for (size_t x=0; x<reference.size(); ++x) {
            const auto actual = pixel(values,surface.fbo,int(x),0);
            for (int c=0; c<3; ++c)
                EXPECT_NEAR(actual[c],reference[x][c],2.0f*quantum) << "patch=" << x << " channel=" << c;
            EXPECT_FLOAT_EQ(actual[3],1.0f);
        }
        // Colorimetric Rec1886 deliberately differs at midgray. The preview
        // exception must be numeric identity rather than the old sRGB(x^2.4).
        EXPECT_GT(std::abs(pixel(values,surface.fbo,3,0)[0]-encodeSrgb(std::pow(0.18f,2.4f))),0.03f);
    }
}

TEST_F(VideoPresenterPixelTest, PreviewIntentOnlyBypassesTheExplicitRec1886UntoneDisplayWhitelist) {
    auto input = source({0.18f,0.18f,0.18f,1},1,1); ASSERT_NE(input,nullptr);
    for (const auto& selection : std::array<ColorOutputRequest,4>{{
             {"sRGB - Display", "Un-tone-mapped", {}, ColorOutputIntent::SdrVideoPreview},
             {{}, {}, "sRGB Encoded Rec.709 (sRGB)", ColorOutputIntent::SdrVideoPreview},
             {"Rec.1886 Rec.709 - Display", "ACES 2.0 - SDR 100 nits (Rec.709)", {}, ColorOutputIntent::SdrVideoPreview},
             {{}, {}, "Rec.1886 Rec.709 - Display", ColorOutputIntent::SdrVideoPreview}}}) {
        SCOPED_TRACE(testing::Message() << selection.display << ' ' << selection.view << ' ' << selection.colorSpace);
        auto surface = target("rgba8",1,1); ASSERT_NE(surface.fbo,nullptr);
        std::string error;
        ASSERT_TRUE(blitPresentationFrame(gpu,*blitter,input,selection,surface,&error)) << error;
        const auto values = download(surface.fbo);
        const bool srgb = selection.display == "sRGB - Display" || selection.colorSpace == "sRGB Encoded Rec.709 (sRGB)";
        const float expected = srgb ? 0.18f : encodeSrgb(std::pow(0.18f,2.4f));
        for (int c=0; c<3; ++c) EXPECT_NEAR(pixel(values,surface.fbo,0,0)[c],expected,2.0f/255.0f);
    }
}

TEST_F(VideoPresenterPixelTest, VideoPreviewIntentDoesNotPermitHdrUnknownOrNonSdrSurfaceContracts) {
    auto input = source({0.18f,0.5f,0.75f,1},1,1); ASSERT_NE(input,nullptr);
    auto surface = target("rgba8",1,1); ASSERT_NE(surface.fbo,nullptr);
    const ColorOutputRequest selection{"Rec.1886 Rec.709 - Display", "Un-tone-mapped", {},
        ColorOutputIntent::SdrVideoPreview};
    std::string error;
    for (const auto& unsupported : std::array<ColorOutputRequest,6>{{
             {{}, {}, {}, ColorOutputIntent::SdrVideoPreview},
             {"unknown display", "Un-tone-mapped", {}, ColorOutputIntent::SdrVideoPreview},
             {"Rec.2100-PQ - Display", "Un-tone-mapped", {}, ColorOutputIntent::SdrVideoPreview},
             {"Rec.2100-HLG - Display", "Un-tone-mapped", {}, ColorOutputIntent::SdrVideoPreview},
             {{}, {}, "ACEScg", ColorOutputIntent::SdrVideoPreview},
             {{}, {}, "Linear Rec.709 (sRGB)", ColorOutputIntent::SdrVideoPreview}}}) {
        EXPECT_FALSE(blitPresentationFrame(gpu,*blitter,input,unsupported,surface,&error));
        EXPECT_FALSE(error.empty());
    }
    for (auto transfer : {PL_COLOR_TRC_UNKNOWN, PL_COLOR_TRC_PQ, PL_COLOR_TRC_HLG, PL_COLOR_TRC_LINEAR, PL_COLOR_TRC_SCRGB}) {
        auto invalid = surface; invalid.color_space.transfer = transfer;
        EXPECT_FALSE(blitPresentationFrame(gpu,*blitter,input,selection,invalid,&error));
        EXPECT_FALSE(error.empty());
    }
    auto wide = surface; wide.color_space.primaries = PL_COLOR_PRIM_BT_2020;
    EXPECT_FALSE(blitPresentationFrame(gpu,*blitter,input,selection,wide,&error));
    auto limited = surface; limited.color_repr.levels = PL_COLOR_LEVELS_LIMITED;
    EXPECT_FALSE(blitPresentationFrame(gpu,*blitter,input,selection,limited,&error));
    auto floating = target("rgba16f",1,1); ASSERT_NE(floating.fbo,nullptr);
    EXPECT_FALSE(blitPresentationFrame(gpu,*blitter,input,selection,floating,&error));
}

TEST_F(VideoPresenterPixelTest, SurfaceFlipPhysicalExtentLetterboxAndComponentOrderAreHonored) {
    const std::vector<float> pixels{
        0.1f,0.2f,0.3f,1, 0.1f,0.2f,0.3f,1, 0.1f,0.2f,0.3f,1, 0.1f,0.2f,0.3f,1,
        0.6f,0.7f,0.8f,1, 0.6f,0.7f,0.8f,1, 0.6f,0.7f,0.8f,1, 0.6f,0.7f,0.8f,1};
    auto input = source(pixels, 4, 2); ASSERT_NE(input, nullptr);
    for (bool flip : {false, true}) {
        auto surface = target("bgra8", 4, 6, flip); ASSERT_NE(surface.fbo, nullptr);
        std::string error;
        ASSERT_TRUE(blitPresentationFrame(gpu, *blitter, input, {"sRGB - Display", "Un-tone-mapped", {}}, surface, &error)) << error;
        auto values = download(surface.fbo);
        // 2:1 content is centered at native size in the actual 4x6 attachment:
        // y=[2,4). No scaler can blur the two asymmetric reference rows.
        for (int y : {0, 1, 4, 5})
            for (int c = 0; c < 3; ++c) EXPECT_NEAR(pixel(values, surface.fbo, 2, y)[c], 0.0f, 1.0f / 255.0f);
        const auto top = pixel(values, surface.fbo, 2, flip ? 3 : 2);
        const auto bottom = pixel(values, surface.fbo, 2, flip ? 2 : 3);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(top[c], 0.1f * (c + 1), 2.0f / 255.0f);
            EXPECT_NEAR(bottom[c], 0.6f + 0.1f * c, 2.0f / 255.0f);
        }
    }
}

TEST_F(VideoPresenterPixelTest, UnsupportedSourceAndHdrScRgbOrUnknownSurfaceFailBeforeRendering) {
    auto input = source({0.5f,0.5f,0.5f,1}, 1, 1); ASSERT_NE(input, nullptr);
    auto surface = target("rgba8", 1, 1); ASSERT_NE(surface.fbo, nullptr);
    const ColorOutputRequest selection{"sRGB - Display", "Un-tone-mapped", {}};
    std::string error;
    for (const ColorOutputRequest unsupported : {ColorOutputRequest{},
         ColorOutputRequest{"Rec.2100-PQ - Display", "Un-tone-mapped", {}},
         ColorOutputRequest{{}, {}, "ACEScg"}}) {
        EXPECT_FALSE(blitPresentationFrame(gpu, *blitter, input, unsupported, surface, &error));
        EXPECT_FALSE(error.empty());
    }
    for (auto transfer : {PL_COLOR_TRC_UNKNOWN, PL_COLOR_TRC_PQ, PL_COLOR_TRC_HLG, PL_COLOR_TRC_LINEAR, PL_COLOR_TRC_SCRGB}) {
        auto invalid = surface; invalid.color_space.transfer = transfer;
        EXPECT_FALSE(blitPresentationFrame(gpu, *blitter, input, selection, invalid, &error));
        EXPECT_FALSE(error.empty());
    }
    auto wide = surface; wide.color_space.primaries = PL_COLOR_PRIM_BT_2020;
    EXPECT_FALSE(blitPresentationFrame(gpu, *blitter, input, selection, wide, &error));
    auto limited = surface; limited.color_repr.levels = PL_COLOR_LEVELS_LIMITED;
    EXPECT_FALSE(blitPresentationFrame(gpu, *blitter, input, selection, limited, &error));
    auto floating = target("rgba16f", 1, 1); ASSERT_NE(floating.fbo, nullptr);
    EXPECT_FALSE(blitPresentationFrame(gpu, *blitter, input, selection, floating, &error));
}
} // namespace
