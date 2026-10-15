#include <volk.h>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <bit>
#include <atomic>
#include <future>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace heisenberg::renderer::color;
using namespace heisenberg::filtergraph;
namespace {
void vkCheck(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Vulkan test operation failed " + std::to_string(int(result))); }
float halfToFloat(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 31u;
    uint32_t mantissa = value & 1023u;
    uint32_t bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else { int shift = 0; while (!(mantissa & 1024u)) { mantissa <<= 1; ++shift; }
               bits = sign | (uint32_t(113 - shift) << 23) | ((mantissa & 1023u) << 13); }
    } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}
class GpuFixture : public testing::Test {
protected:
    VulkanGraphContext context{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    uint64_t validationBaseline = 0;
    bool sharedInitialized = false;
    std::atomic<unsigned> validationErrors{0};
    std::atomic<unsigned> validationProbes{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto& fixture = *static_cast<GpuFixture*>(user);
        if (data->pMessageIdName && std::strcmp(data->pMessageIdName, "ColorValidationProbe") == 0)
            ++fixture.validationProbes;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++fixture.validationErrors;
            std::cerr << "Color Validation Error: " << data->pMessage << '\n';
        }
        return VK_FALSE;
    }
    void SetUp() override {
        auto& shared = heisenberg::renderer::VulkanContext::instance();
        shared.initLoader();
        uint32_t layerCount = 0;
        vkCheck(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
        std::vector<VkLayerProperties> layers(layerCount);
        vkCheck(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));
        const bool validationAvailable = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        // Own only this fixture's pool/resources. Never replace process-wide
        // Volk dispatch or destroy the singleton instance/device used by Grade,
        // Interop, libplacebo and subsequent tests in this same process.
        const bool existingInstance = bool(shared.vkInstance());
        const char* layerEnvironment = std::getenv("VK_INSTANCE_LAYERS");
        const bool validationEnvironment = layerEnvironment && std::strstr(layerEnvironment, "VK_LAYER_KHRONOS_validation");
        const bool validationExpected = existingInstance ? validationEnvironment : validationAvailable;
        shared.createInstance(validationAvailable);
        shared.createDevice();
        sharedInitialized = true;
        context.instance = shared.vkInstance();
        context.physicalDevice = shared.physicalDevice();
        context.device = shared.device();
        context.queue = shared.graphicsQueue();
        context.queueFamilyIndex = shared.graphicsQueueFamily();
        validationBaseline = shared.validationErrorCount();
        ASSERT_EQ(volkGetLoadedInstance(), context.instance);
        ASSERT_EQ(volkGetLoadedDevice(), context.device);
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            shared.getInstanceProcAddr()(context.instance, "vkCreateDebugUtilsMessengerEXT"));
        const auto submitMessage = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
            shared.getInstanceProcAddr()(context.instance, "vkSubmitDebugUtilsMessageEXT"));
        if (validationExpected && createMessenger && submitMessage) {
            VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = validationCallback; debug.pUserData = this;
            vkCheck(createMessenger(context.instance, &debug, nullptr, &messenger));
            VkDebugUtilsMessengerCallbackDataEXT probe{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            probe.pMessageIdName = "ColorValidationProbe"; probe.pMessage = "Color validation callback active";
            submitMessage(context.instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &probe);
            ASSERT_EQ(validationProbes.load(), 1u);
            std::cout << "Color validation: shared VulkanContext; callback probe received; baseline=" << validationBaseline << std::endl;
        } else std::cout << "Color validation: shared context has no debug messenger (not validation evidence)" << std::endl;
        VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; commandPool.queueFamilyIndex = context.queueFamilyIndex;
        vkCheck(vkCreateCommandPool(context.device, &commandPool, nullptr, &pool));
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(context.physicalDevice, &props);
        std::cout << "Color GPU device: " << props.deviceName << '\n';
    }
    void TearDown() override {
        if (context.device) {
            heisenberg::renderer::synchronizedQueueWaitIdle(context.queue);
            if (pool) vkDestroyCommandPool(context.device, pool, nullptr);
        }
        if (messenger) {
            std::cout << "Color validation errors: " << validationErrors.load() << std::endl;
            EXPECT_EQ(validationErrors.load(), 0u);
            vkDestroyDebugUtilsMessengerEXT(context.instance, messenger, nullptr);
        }
        if (sharedInitialized) {
            auto& shared = heisenberg::renderer::VulkanContext::instance();
            EXPECT_EQ(shared.validationErrorCount(), validationBaseline);
            EXPECT_EQ(volkGetLoadedInstance(), static_cast<VkInstance>(shared.vkInstance()));
            EXPECT_EQ(volkGetLoadedDevice(), static_cast<VkDevice>(shared.device()));
            // This call also exercises the shared context's foreign-dispatch
            // guard before the following Grade/Interop tests use libplacebo.
            EXPECT_NO_THROW(shared.createDevice());
        }
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{}; vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No test memory type");
    }
    struct Image {
        VkDevice device;
        VulkanImageRef ref;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        explicit Image(VkDevice d) : device(d) {}
        ~Image() { if (ref.view) vkDestroyImageView(device, ref.view, nullptr); if (ref.image) vkDestroyImage(device, ref.image, nullptr); if (memory) vkFreeMemory(device, memory, nullptr); }
    };
    std::unique_ptr<Image> image(uint32_t width, VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t height = 1) {
        auto result = std::make_unique<Image>(context.device);
        auto& ref = result->ref; ref.extent = {width, height}; ref.vkFormat = format;
        ref.queueFamilyIndex = context.queueFamilyIndex;
        ref.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D; info.format = ref.vkFormat;
        info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = ref.usage;
        vkCheck(vkCreateImage(context.device, &info, nullptr, &ref.image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(context.device, ref.image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(context.device, &alloc, nullptr, &result->memory)); vkCheck(vkBindImageMemory(context.device, ref.image, result->memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = ref.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = ref.vkFormat; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(context.device, &view, nullptr, &ref.view)); return result;
    }
    void transfer(Image& image, std::vector<float>& pixels, bool upload) {
        VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
        const bool half = image.ref.vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
        if (half && upload) throw std::invalid_argument("Test uploads FP32 inputs; FP16 only used for output");
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = pixels.size() * (half ? sizeof(uint16_t) : sizeof(float));
        info.usage = upload ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCheck(vkCreateBuffer(context.device, &info, nullptr, &buffer));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(context.device, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(context.device, &alloc, nullptr, &memory)); vkCheck(vkBindBufferMemory(context.device, buffer, memory, 0));
        void* mapped = nullptr; vkCheck(vkMapMemory(context.device, memory, 0, info.size, 0, &mapped));
        if (upload) std::memcpy(mapped, pixels.data(), size_t(info.size));
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; command.commandPool = pool; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(context.device, &command, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkCheck(vkBeginCommandBuffer(cmd, &begin));
        auto transition = [&](VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags src, VkAccessFlags dst) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.ref.image;
            barrier.oldLayout = oldLayout; barrier.newLayout = newLayout; barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };
        const auto transferLayout = upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        transition(image.ref.layout, transferLayout, image.ref.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                   upload ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {image.ref.extent.width, image.ref.extent.height, 1};
        if (upload) vkCmdCopyBufferToImage(cmd, buffer, image.ref.image, transferLayout, 1, &copy);
        else vkCmdCopyImageToBuffer(cmd, image.ref.image, transferLayout, buffer, 1, &copy);
        transition(transferLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, upload ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCheck(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        VkFence fence = VK_NULL_HANDLE; VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCheck(vkCreateFence(context.device, &fenceInfo, nullptr, &fence));
        vkCheck(heisenberg::renderer::synchronizedQueueSubmit(context.queue, 1, &submit, fence)); vkCheck(vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX));
        if (!upload) {
            if (half) {
                const auto* values = static_cast<const uint16_t*>(mapped);
                for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = halfToFloat(values[i]);
            } else std::memcpy(pixels.data(), mapped, size_t(info.size));
        }
        vkUnmapMemory(context.device, memory); vkDestroyFence(context.device, fence, nullptr); vkFreeCommandBuffers(context.device, pool, 1, &cmd);
        vkDestroyBuffer(context.device, buffer, nullptr); vkFreeMemory(context.device, memory, nullptr);
        image.ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    void compare(const ColorProcessor& processor, const std::vector<float>& input, float tolerance, const char* label,
                 VkFormat outputFormat = VK_FORMAT_R32G32B32A32_SFLOAT) {
        auto cpu = input; processor.applyRGBA(cpu.data(), cpu.size() / 4);
        auto in = image(uint32_t(input.size() / 4)), out = image(uint32_t(input.size() / 4), outputFormat);
        auto pixels = input; transfer(*in, pixels, true);
        ColorGpuPass pass(context);
        const auto program = processor.gpuProgram();
        auto ready = pass.execute(program, in->ref, out->ref);
        ASSERT_TRUE(ready.valid()); out->ref.ready = ready; out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        uint64_t value = 0; vkCheck(vkGetSemaphoreCounterValue(context.device, ready.semaphore, &value)); EXPECT_GE(value, ready.value);
        transfer(*out, pixels, false);
        float maxError = 0;
        for (size_t i = 0; i < pixels.size(); ++i) {
            ASSERT_TRUE(std::isfinite(pixels[i])) << label << " channel " << i;
            if (i % 4 == 3) EXPECT_FLOAT_EQ(pixels[i], input[i]);
            else { maxError = std::max(maxError, std::abs(pixels[i] - cpu[i])); EXPECT_NEAR(pixels[i], cpu[i], tolerance * std::max(1.0f, std::abs(cpu[i]))) << label << " channel " << i; }
        }
        std::cout << label << " GPU/CPU max absolute error=" << maxError << ", LUTs=" << program.luts.size() << ", UBO bytes=" << program.uniformData.size() << '\n';
    }
};
std::vector<float> samples() { return {-0.01f, 0.0f, 0.001f, 0.0f, 0.005f, 0.0078125f, 0.02f, 0.25f, 0.18f, 0.3f, 0.5f, 0.5f, 1.0f, 2.0f, 4.0f, 0.75f, 8.0f, 16.0f, 32.0f, 1.0f, -0.1f, 1.5f, 0.4f, -0.5f}; }
class TemporaryLut {
public:
    std::filesystem::path path;
    explicit TemporaryLut(const char* suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("heisenberg_color_" + std::to_string(stamp) + suffix);
    }
    ~TemporaryLut() { std::error_code error; std::filesystem::remove(path, error); }
};
}
TEST(ColorPipelineCpu, ResolvesOverrideMetadataAndTraceableFallback) {
    ColorPipeline pipeline;
    heisenberg::VideoSpec metadata; metadata.format = AV_PIX_FMT_RGBA;
    const auto fallback = pipeline.resolveInput(metadata);
    EXPECT_EQ(fallback.source, InputSelectionSource::Fallback); EXPECT_FALSE(fallback.diagnostics.empty());
    EXPECT_EQ(fallback.colorSpace, "sRGB - Display");
    EXPECT_EQ(pipeline.resolveInput(metadata, {}, 1).colorSpace, "sRGB Encoded Rec.709 (sRGB)");
    metadata.colorPrimaries = AVCOL_PRI_BT709; metadata.colorTransfer = AVCOL_TRC_BT709;
    const auto observed = pipeline.resolveInput(metadata);
    EXPECT_EQ(observed.source, InputSelectionSource::Metadata); EXPECT_EQ(observed.colorSpace, kExportDisplay);
    EXPECT_EQ(pipeline.resolveInput(metadata, {}, 1).colorSpace, "Camera Rec.709");
    EXPECT_EQ(observed.observedMetadata.colorTransfer, AVCOL_TRC_BT709);
    EXPECT_EQ(observed.diagnostics.size(), 1u); // Unspecified range still needs a warning.
    metadata.colorRange = AVCOL_RANGE_MPEG;
    EXPECT_TRUE(pipeline.resolveInput(metadata).diagnostics.empty());
    const auto overridden = pipeline.resolveInput(metadata, "ACEScg");
    EXPECT_EQ(overridden.source, InputSelectionSource::Override);
    EXPECT_TRUE(overridden.diagnostics.empty());
    EXPECT_THROW(pipeline.resolveInput(metadata, "does-not-exist"), std::invalid_argument);
}
TEST(ColorPipelineCpu, VersionedSdrMappingsOverridesAndUnchangedHdrAndLinear) {
    ColorPipeline pipeline;
    heisenberg::VideoSpec metadata; metadata.format = AV_PIX_FMT_NV12;
    for (const auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_D3D11}) {
        metadata.format = format;
        const auto current = pipeline.resolveInput(metadata);
        EXPECT_EQ(current.source, InputSelectionSource::Fallback);
        EXPECT_EQ(current.colorSpace, kExportDisplay);
        EXPECT_FALSE(current.diagnostics.empty());
        EXPECT_EQ(pipeline.resolveInput(metadata, {}, 1).colorSpace, "Camera Rec.709");
    }
    metadata.colorPrimaries = AVCOL_PRI_BT709; metadata.colorRange = AVCOL_RANGE_MPEG;
    const std::array<std::pair<AVColorTransferCharacteristic, const char*>, 4> mappings{{
        {AVCOL_TRC_BT709, kExportDisplay}, {AVCOL_TRC_SMPTE170M, kExportDisplay},
        {AVCOL_TRC_IEC61966_2_1, kPreviewDisplay}, {AVCOL_TRC_GAMMA22, "Gamma 2.2 Rec.709 - Display"}}};
    for (const auto& [transfer, space] : mappings) {
        metadata.colorTransfer = transfer;
        EXPECT_EQ(pipeline.resolveInput(metadata).colorSpace, space);
        for (const int policy : {1,2}) {
            const auto explicitCamera = pipeline.resolveInput(metadata, "Camera Rec.709", policy);
            EXPECT_EQ(explicitCamera.colorSpace, "Camera Rec.709");
            EXPECT_EQ(explicitCamera.source, InputSelectionSource::Override);
            EXPECT_TRUE(explicitCamera.diagnostics.empty());
        }
    }
    metadata.colorTransfer = AVCOL_TRC_LINEAR;
    EXPECT_EQ(pipeline.resolveInput(metadata).colorSpace, "Linear Rec.709 (sRGB)");
    metadata.colorPrimaries = AVCOL_PRI_BT2020;
    for (const auto transfer : {AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67, AVCOL_TRC_LINEAR}) {
        metadata.colorTransfer = transfer;
        EXPECT_EQ(pipeline.resolveInput(metadata, {}, 1).colorSpace, pipeline.resolveInput(metadata, {}, 2).colorSpace);
    }
    EXPECT_THROW(pipeline.resolveInput(metadata, {}, 3), std::invalid_argument);
}

TEST(ColorPipelineCpu, FinishedSdrColorimetricRoundTripAndIndependentSrgbEncoding) {
    ColorPipeline pipeline;
    // Independent source codes: grayscale including the sRGB toe, and RGB/CMY
    // bars. No grade or OCIO processor supplies the expected output values.
    std::vector<float> input;
    for (const float code : {0.0f, 0.005f, 0.02f, 0.06f, 0.08f, 0.09f, 0.18f, 0.5f, 0.75f, 1.0f})
        input.insert(input.end(), {code,code,code,0.75f});
    // 75% amplitude bars with a 7.5% pedestal avoid using gamma-amplified
    // floating-point zero residue as the colorimetric accuracy criterion.
    constexpr std::array<std::array<float,3>,6> bars{{
        {0.75f,0.075f,0.075f}, {0.075f,0.75f,0.075f}, {0.075f,0.075f,0.75f},
        {0.075f,0.75f,0.75f}, {0.75f,0.075f,0.75f}, {0.75f,0.75f,0.075f}}};
    for (const auto& bar : bars) input.insert(input.end(), {bar[0],bar[1],bar[2],0.75f});
    constexpr std::array<std::array<float,3>,6> zeroBars{{
        {1,0,0}, {0,1,0}, {0,0,1}, {0,1,1}, {1,0,1}, {1,1,0}}};
    for (const auto& bar : zeroBars) input.insert(input.end(), {bar[0],bar[1],bar[2],0.75f});
    auto working = input;
    pipeline.processor(kExportDisplay,kWorkingSpace).applyRGBA(working.data(),working.size()/4);
    auto exported = working, preview = working;
    pipeline.displayProcessor(kWorkingSpace,kExportDisplay,kColorimetricView).applyRGBA(exported.data(),exported.size()/4);
    pipeline.displayProcessor().applyRGBA(preview.data(),preview.size()/4);
    for (size_t i=0; i<input.size(); ++i) {
        SCOPED_TRACE(i);
        if (i%4 == 3) { EXPECT_FLOAT_EQ(exported[i],input[i]); EXPECT_FLOAT_EQ(preview[i],input[i]); continue; }
        // At a pure primary's zero component, the inverse gamma magnifies
        // tiny signed matrix residue. Assert the actual display-linear energy
        // (<1e-6) rather than allowing arbitrary encoded near-black drift.
        if (input[i] == 0) {
            const double actualLinear=std::copysign(std::pow(std::abs(double(exported[i])),2.4),double(exported[i]));
            EXPECT_NEAR(actualLinear,0.0,0.000001);
        } else EXPECT_NEAR(exported[i],input[i],0.00004f);
        const double linear = std::pow(double(input[i]),2.4);
        const double expected = linear <= 0.0031308 ? 12.92*linear : 1.055*std::pow(linear,1.0/2.4)-0.055;
        // sRGB's linear toe is explicitly tested (rather than treating it as
        // gamma 2.2); 6e-5 allows only OCIO float/matrix approximation error.
        EXPECT_NEAR(preview[i],expected,0.00006);
    }
}

TEST(ColorPipelineCpu, ExplicitAcesViewRetainsPinnedAces20OdtValues) {
    ColorPipeline pipeline;
    // Independent OCIO 2.5.2 BuiltinTransform_tests ACES 2 SDR 100nit vector:
    // ACES2065-1 (0.5,0.4,0.3) -> XYZ-D65 (0.26260215,0.25207460,0.20617345).
    // Published XYZ -> linear Rec.709 D65 matrix plus independent display EOTF.
    constexpr double xyz[]{0.26260215,0.25207460,0.20617345};
    constexpr double matrix[3][3]{{3.2409699419,-1.5373831776,-0.4986107603},
        {-0.9692436363,1.8759675015,0.0415550574}, {0.0556300797,-0.2039769589,1.0569715142}};
    float working[]{0.5f,0.4f,0.3f,0.75f};
    pipeline.processor("ACES2065-1",kWorkingSpace).applyRGBA(working,1);
    auto preview = std::array<float,4>{working[0],working[1],working[2],working[3]};
    auto exported = preview;
    pipeline.displayProcessor(kWorkingSpace,kPreviewDisplay,kAcesOutputView).applyRGBA(preview.data(),1);
    pipeline.displayProcessor(kWorkingSpace,kExportDisplay,kAcesOutputView).applyRGBA(exported.data(),1);
    for (int c=0; c<3; ++c) {
        double linear = 0; for (int j=0; j<3; ++j) linear += matrix[c][j]*xyz[j];
        EXPECT_NEAR(exported[c],std::pow(linear,1.0/2.4),0.0003);
        EXPECT_NEAR(preview[c],1.055*std::pow(linear,1.0/2.4)-0.055,0.0003);
    }
    EXPECT_FLOAT_EQ(preview[3],0.75f); EXPECT_FLOAT_EQ(exported[3],0.75f);
}

TEST(ColorPipelineCpu, AcescctRoundTripPreservesNegativeSuperwhiteAndAlpha) {
    ColorPipeline pipeline; auto input = samples(); auto output = input;
    pipeline.processor(kWorkingSpace, kGradingSpace).applyRGBA(output.data(), output.size() / 4);
    pipeline.processor(kGradingSpace, kWorkingSpace).applyRGBA(output.data(), output.size() / 4);
    for (size_t i = 0; i < input.size(); ++i)
        EXPECT_NEAR(output[i], input[i], 0.0001f * std::max(1.0f, std::abs(input[i])));
    EXPECT_LT(output[0], 0); EXPECT_GT(output[18], 1); EXPECT_FLOAT_EQ(output[23], -0.5f);
}
TEST(ColorPipelineCpu, OutputSelectionsHaveCorrectDistinctTransferEncoding) {
    ColorPipeline pipeline; float preview[]{0.18f, 0.18f, 0.18f, 0.7f}; float output[]{0.18f, 0.18f, 0.18f, 0.7f};
    pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView).applyRGBA(preview, 1);
    pipeline.displayProcessor(kWorkingSpace, kExportDisplay, kAcesOutputView).applyRGBA(output, 1);
    EXPECT_GT(preview[0], 0); EXPECT_NE(preview[0], output[0]); EXPECT_FLOAT_EQ(preview[3], 0.7f); EXPECT_FLOAT_EQ(output[3], 0.7f);
    EXPECT_THROW(pipeline.displayProcessor(kWorkingSpace, "missing display", kOutputView), std::exception);
    EXPECT_THROW(ColorPipeline(VulkanGraphContext{}, "other-config"), std::invalid_argument);
}
TEST_F(GpuFixture, AcescgAcescctBothDirectionsAndEncodedInput) {
    ColorPipeline pipeline;
    compare(pipeline.processor(kWorkingSpace, kGradingSpace), samples(), 0.0001f, "ACEScg->ACEScct");
    auto cct = samples(); pipeline.processor(kWorkingSpace, kGradingSpace).applyRGBA(cct.data(), cct.size() / 4);
    compare(pipeline.processor(kGradingSpace, kWorkingSpace), cct, 0.0002f, "ACEScct->ACEScg");
    compare(pipeline.processor("Camera Rec.709", kWorkingSpace), samples(), 0.0002f, "encoded Rec.709->ACEScg");
}
TEST_F(GpuFixture, PreviewAndExportOdtMatchCpuIncludingOcioUniforms) {
    ColorPipeline pipeline;
    compare(pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView), samples(), 0.00002f, "preview ODT");
    compare(pipeline.displayProcessor(kWorkingSpace, kExportDisplay, kAcesOutputView), samples(), 0.00002f, "export ODT");
}
TEST_F(GpuFixture, Rgba16fOwnedOutputPreservesExtendedRangeAndMatchesOdt) {
    ColorPipeline pipeline;
    const auto format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Alpha sample values are exactly half-representable, so bit-preservation
    // is asserted even though RGB quantization has a distinct FP16 tolerance.
    compare(pipeline.processor(kWorkingSpace, kWorkingSpace), samples(), 0.0006f, "RGBA16F extended-range identity", format);
    compare(pipeline.processor(kWorkingSpace, kGradingSpace), samples(), 0.0006f, "RGBA16F ACEScct", format);
    compare(pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView), samples(), 0.0006f, "RGBA16F preview ODT", format);
    compare(pipeline.displayProcessor(kWorkingSpace, kExportDisplay, kAcesOutputView), samples(), 0.0006f, "RGBA16F export ODT", format);
}
TEST_F(GpuFixture, Arbitrary2dShaperAnd3dLutUploadMatchCpu) {
    ColorPipeline pipeline;
    TemporaryLut shaper(".cube");
    { std::ofstream file(shaper.path); file << "LUT_1D_SIZE 5\n0 0 0\n0.1 0.2 0.3\n0.4 0.5 0.6\n0.7 0.8 0.9\n1 1 1\n"; }
    auto oneD = pipeline.lutProcessor(shaper.path.string());
    auto program = oneD.gpuProgram(); ASSERT_FALSE(program.luts.empty()); EXPECT_FALSE(program.luts[0].is3D);
    // Hardware linear sampling fractions have implementation-defined finite
    // subtexel precision (e.g. 8 bits), independent of FP32 LUT storage.
    compare(oneD, {0.1f, 0.2f, 0.3f, 0.4f, 0.6f, 0.7f, 0.8f, 0.9f}, 0.001f, "1D LUT as 2D texture");
    TemporaryLut cube(".cube");
    { std::ofstream file(cube.path); file << "LUT_3D_SIZE 3\n";
      for (int b = 0; b < 3; ++b) for (int g = 0; g < 3; ++g) for (int r = 0; r < 3; ++r)
          file << r * 0.4f + g * 0.05f << ' ' << g * 0.35f + b * 0.1f << ' ' << b * 0.3f + r * 0.15f << '\n'; }
    auto threeD = pipeline.lutProcessor(cube.path.string()); program = threeD.gpuProgram();
    ASSERT_FALSE(program.luts.empty()); EXPECT_TRUE(program.luts[0].is3D); EXPECT_EQ(program.luts[0].width, 3u);
    compare(threeD, {0.13f, 0.27f, 0.71f, 0.0f, 0.91f, 0.64f, 0.38f, 0.8f}, 0.0001f, "non-64 3D LUT");
}
TEST_F(GpuFixture, DynamicOcioUniformBufferUsesActualOffsetsAndBindingZero) {
    ColorPipeline pipeline;
    TemporaryLut transform(".ctf");
    { std::ofstream file(transform.path);
      file << "<?xml version=\"1.0\"?><ProcessList id=\"color-ubo\" version=\"2.0\">"
              "<ExposureContrast inBitDepth=\"32f\" outBitDepth=\"32f\" style=\"linear\">"
              "<ECParams exposure=\"0.65\" contrast=\"1.2\" gamma=\"0.8\" pivot=\"0.18\"/>"
              "<DynamicParameter param=\"EXPOSURE\"/><DynamicParameter param=\"CONTRAST\"/>"
              "<DynamicParameter param=\"GAMMA\"/></ExposureContrast></ProcessList>"; }
    auto processor = pipeline.lutProcessor(transform.path.string());
    const auto program = processor.gpuProgram();
    ASSERT_GE(program.uniformData.size(), 12u);
    EXPECT_NE(program.shaderText.find("binding = 0"), std::string::npos);
    EXPECT_NE(program.shaderText.find("set = 1"), std::string::npos);
    compare(processor, samples(), 0.0002f, "dynamic exposure/contrast UBO");
}
TEST_F(GpuFixture, CancellationPollsSubmittedWorkAndDrainsBeforeReturning) {
    ColorPipeline pipeline(context);
    auto in = image(1), out = image(1);
    std::vector<float> pixels{-0.125f, 2.0f, 0.5f, 0.75f}; transfer(*in, pixels, true);
    pipeline.setCancellation([] { return true; });
    std::string error;
    EXPECT_FALSE(pipeline.executeSpace(in->ref, out->ref, kWorkingSpace, kWorkingSpace, &error).valid());
    EXPECT_EQ(error, "Cancelled"); // cancelled before submission: output untouched

    pipeline.setCancellation({});
    ASSERT_TRUE(pipeline.executeSpace(in->ref, out->ref, kWorkingSpace, kWorkingSpace, &error).valid()) << error;
    out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkSemaphore gate = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext = &type;
    vkCheck(vkCreateSemaphore(context.device, &info, nullptr, &gate));
    in->ref.ready = {gate, 1};
    std::atomic<int> observations{0};
    // Warm identity has two pre-submit checks (entry, pre-submit).
    // Latch cancellation on the first fence-poll callback, after submission.
    pipeline.setCancellation([&] { return observations.fetch_add(1) + 1 >= 3; });
    auto task = std::async(std::launch::async, [&] {
        std::string detail;
        const auto ready = pipeline.executeSpace(in->ref, out->ref, kWorkingSpace, kWorkingSpace, &detail);
        return std::pair{ready, detail};
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observations.load() < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool observedMultiplePolls = observations.load() >= 4;
    const bool remainedInFlight = task.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
    // Always unblock the submitted GPU wait before any fatal assertion/teardown.
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
    vkCheck(vkSignalSemaphore(context.device, &signal));
    const auto result = task.get();
    vkDestroySemaphore(context.device, gate, nullptr); in->ref.ready = {};
    EXPECT_TRUE(observedMultiplePolls);
    EXPECT_TRUE(remainedInFlight) << "Cancelled submission must not return before its fence drains";
    EXPECT_FALSE(result.first.valid()); EXPECT_EQ(result.second, "Cancelled");
    out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto downloaded = pixels; transfer(*out, downloaded, false);
    EXPECT_EQ(downloaded, pixels); // proves the gated GPU dispatch actually ran
    pipeline.setCancellation({});
    const auto ready = pipeline.executeSpace(in->ref, out->ref, kWorkingSpace, kWorkingSpace, &error);
    EXPECT_TRUE(ready.valid()) << error;
}
TEST_F(GpuFixture, ExplicitErrorsNeverSubstituteCpuAndRestoreInputLayout) {
    ColorPipeline cpu; std::string error;
    EXPECT_FALSE(cpu.executeSpace({}, {}, kWorkingSpace, kGradingSpace, &error).valid()); EXPECT_FALSE(error.empty());
    ColorPipeline pipeline(context); auto in = image(1), out = image(1); auto pixels = std::vector<float>{-0.1f, 2.0f, 0.3f, 0.6f}; transfer(*in, pixels, true);
    auto ready = pipeline.executeSpace(in->ref, out->ref, kWorkingSpace, kWorkingSpace, &error);
    ASSERT_TRUE(ready.valid()) << error; EXPECT_TRUE(error.empty());
    out->ref.ready = ready; out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto restored = pixels; transfer(*in, restored, false); EXPECT_EQ(restored, pixels);
    transfer(*out, restored, false); EXPECT_EQ(restored, pixels);
    EXPECT_FALSE(pipeline.executeSpace(in->ref, in->ref, kWorkingSpace, kGradingSpace, &error).valid()); EXPECT_FALSE(error.empty());
    auto srgb = in->ref; srgb.vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
    EXPECT_FALSE(pipeline.executeSpace(srgb, out->ref, "sRGB Encoded Rec.709 (sRGB)", kWorkingSpace, &error).valid());
    EXPECT_NE(error.find("duplicate transfer"), std::string::npos);
}

TEST(ColorPipelineCpu, ReusesImmutableShaderAndLutSnapshots) {
    ColorPipeline pipeline;
    auto processor = pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView);
    auto first = processor.gpuProgramSnapshot();
    for (int i = 0; i < 24; ++i)
        EXPECT_EQ(pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView).gpuProgramSnapshot(), first);
    EXPECT_FALSE(first->luts.empty());
}
TEST_F(GpuFixture, ProgramCacheReusesLutsIsBoundedAndSeparatesOutputFormats) {
    ColorPipeline pipeline; ColorGpuPass pass(context);
    auto in = image(1), out = image(1);
    std::vector<float> pixels{0.18f, 0.3f, 0.5f, 0.75f}; transfer(*in, pixels, true);
    const auto program = pipeline.displayProcessor(kWorkingSpace, kPreviewDisplay, kAcesOutputView).gpuProgramSnapshot();
    for (int i = 0; i < 24; ++i) {
        out->ref.ready = pass.execute(*program, in->ref, out->ref);
        out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    auto stats = pass.cacheStats();
    EXPECT_EQ(stats.shaderCompilations, 1u); EXPECT_EQ(stats.lutUploads, program->luts.size());
    EXPECT_EQ(stats.hits, 23u); EXPECT_EQ(stats.residentPrograms, 1u);
    auto half = image(1, VK_FORMAT_R16G16B16A16_SFLOAT);
    pass.execute(*program, in->ref, half->ref);
    EXPECT_EQ(pass.cacheStats().shaderCompilations, 2u);
    ColorGpuPass independent(context);
    EXPECT_EQ(independent.cacheStats().residentPrograms, 0u);
    auto identity = pipeline.processor(kWorkingSpace, kWorkingSpace).gpuProgram();
    for (size_t i = 0; i < kProgramCacheCapacity; ++i) {
        identity.cacheId = "lru-test-" + std::to_string(i);
        pass.execute(identity, in->ref, out->ref);
    }
    stats = pass.cacheStats(); EXPECT_EQ(stats.residentPrograms, kProgramCacheCapacity); EXPECT_EQ(stats.evictions, 2u);
    pass.execute(*program, in->ref, out->ref);
    EXPECT_EQ(pass.cacheStats().shaderCompilations, stats.shaderCompilations + 1);
    EXPECT_EQ(pass.cacheStats().lutUploads, stats.lutUploads + program->luts.size());
}
TEST_F(GpuFixture, CachedDynamicUniformChangesWithoutRecompiling) {
    ColorPipeline pipeline; TemporaryLut file(".ctf");
    { std::ofstream output(file.path); output << R"(<?xml version="1.0"?><ProcessList id="cache-ubo" version="2.0"><ExposureContrast inBitDepth="32f" outBitDepth="32f" style="linear"><ECParams exposure="0" contrast="1" gamma="1" pivot="0.18"/><DynamicParameter param="EXPOSURE"/></ExposureContrast></ProcessList>)"; }
    auto program = pipeline.lutProcessor(file.path.string()).gpuProgram();
    ASSERT_EQ(program.uniformData.size(), 4u);
    float exposure = 0;
    program.uniformUpdate = [&] { std::vector<std::byte> bytes(4); std::memcpy(bytes.data(), &exposure, 4); return bytes; };
    ColorGpuPass pass(context); auto in = image(1), out = image(1);
    std::vector<float> pixels{0.125f, 0.25f, 0.5f, 0.75f}; transfer(*in, pixels, true);
    for (int i = 0; i < 2; ++i) {
        exposure = float(i);
        out->ref.ready = pass.execute(program, in->ref, out->ref); out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto actual = pixels; transfer(*out, actual, false);
        for (size_t c = 0; c < 3; ++c) EXPECT_NEAR(actual[c], pixels[c] * (i ? 2.0f : 1.0f), 0.00002f);
        EXPECT_FLOAT_EQ(actual[3], pixels[3]);
    }
    EXPECT_EQ(pass.cacheStats().shaderCompilations, 1u); EXPECT_EQ(pass.cacheStats().hits, 1u);
}
TEST_F(GpuFixture, TwentyFourFrameColorExportWorkloadUsesWarmPrograms) {
    ColorPipeline pipeline;
    auto in = image(320, VK_FORMAT_R32G32B32A32_SFLOAT, 180);
    auto out = image(320, VK_FORMAT_R16G16B16A16_SFLOAT, 180);
    std::vector<float> pixels(320 * 180 * 4, 0.18f); transfer(*in, pixels, true);
    ColorGpuPass warm(context);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 24; ++i) {
        const auto program = pipeline.displayProcessor(kWorkingSpace, kExportDisplay, kAcesOutputView).gpuProgramSnapshot();
        out->ref.ready = warm.execute(*program, in->ref, out->ref); out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const auto warmEnd = std::chrono::steady_clock::now();
    const auto program = pipeline.displayProcessor(kWorkingSpace, kExportDisplay, kAcesOutputView).gpuProgramSnapshot();
    for (int i = 0; i < 24; ++i) {
        ColorGpuPass cold(context);
        out->ref.ready = cold.execute(*program, in->ref, out->ref); out->ref.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // cold's semaphore belongs to cold; completed output needs no wait now.
        out->ref.ready = {};
    }
    const auto end = std::chrono::steady_clock::now();
    EXPECT_EQ(warm.cacheStats().shaderCompilations, 1u);
    EXPECT_EQ(warm.cacheStats().lutUploads, program->luts.size()); EXPECT_EQ(warm.cacheStats().hits, 23u);
    std::cout << "24-frame 320x180 FP16 export ODT: cached="
              << std::chrono::duration<double, std::milli>(warmEnd - begin).count()
              << "ms, fresh-pass=" << std::chrono::duration<double, std::milli>(end - warmEnd).count() << "ms\n";
}
