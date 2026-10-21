#include <volk.h>
#include <MultiMedia/Video/Renderer/FilterGraph/Interface/IPipeGraph.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanFilterGraph.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Nodes/VulkanProgramNode.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Nodes/VulkanNode.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Resources/VulkanImageResource.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <gtest/gtest.h>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace {
using namespace heisenberg::filtergraph;
namespace color = heisenberg::renderer::color;
constexpr GraphImageContract cctContract{toFormatId(ImageType::rgba16f),
    ColorPrimaries::aces_ap1, ColorTransfer::acescct, AlphaMode::straight};

std::shared_ptr<GpuProgram> gainProgram(float gain = 1.0f) {
    auto result = std::make_shared<GpuProgram>();
    result->cacheId = "graph-test-gain";
    result->functionName = "gain";
    result->shaderText = "layout(std140,set=1,binding=0) uniform Params {float gainValue;};"
        "vec4 gain(vec4 c){return vec4(c.rgb*gainValue,0.0);}";
    result->uniformData.resize(sizeof(float));
    std::memcpy(result->uniformData.data(), &gain, sizeof(float));
    return result;
}
void check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("GPU graph test VkResult " + std::to_string(int(result)));
}
float halfFloat(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 31u, mantissa = value & 1023u, bits;
    if (!exponent) {
        if (!mantissa) bits = sign;
        else { int shift = 0; while (!(mantissa & 1024u)) { mantissa <<= 1; ++shift; }
            bits = sign | (uint32_t(113-shift) << 23) | ((mantissa & 1023u) << 13); }
    } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
    else bits = sign | ((exponent+112u) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}
class GpuProgramGraphTest : public testing::Test {
protected:
    VulkanGraphContext context{};
    VkCommandPool pool = VK_NULL_HANDLE;
    uint64_t validationBaseline = 0;
    void SetUp() override {
        auto& shared = heisenberg::renderer::VulkanContext::instance();
        try {
            shared.initLoader();
            if (!shared.vkInstance()) shared.createInstance(false);
            if (!shared.device()) shared.createDevice();
            context = {shared.vkInstance(), shared.physicalDevice(), shared.device(),
                shared.graphicsQueue(), shared.graphicsQueueFamily()};
            validationBaseline = shared.validationErrorCount();
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            info.queueFamilyIndex = context.queueFamilyIndex;
            check(vkCreateCommandPool(context.device, &info, nullptr, &pool));
        } catch (const std::exception& e) { GTEST_SKIP() << "No Vulkan GPU: " << e.what(); }
    }
    void TearDown() override {
        if (context.device) {
            heisenberg::renderer::synchronizedQueueWaitIdle(context.queue);
            if (pool) vkDestroyCommandPool(context.device, pool, nullptr);
            EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(), validationBaseline);
        }
    }
    template<class Record> void submit(Record record) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(context.device, &alloc, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin)); record(cmd); check(vkEndCommandBuffer(cmd));
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device, &info, nullptr, &fence));
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.commandBufferCount = 1; submission.pCommandBuffers = &cmd;
        check(heisenberg::renderer::synchronizedQueueSubmit(context.queue, 1, &submission, fence));
        check(vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(context.device, fence, nullptr);
        vkFreeCommandBuffers(context.device, pool, 1, &cmd);
    }
    static void transition(VkCommandBuffer cmd, VulkanImageRef ref, VkImageLayout next) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = ref.image; barrier.oldLayout = ref.layout; barrier.newLayout = next;
        barrier.srcAccessMask = ref.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    std::unique_ptr<VulkanImageResource> image(std::array<float,4> pixel) {
        auto result = std::make_unique<VulkanImageResource>(context);
        if (!result->ensure({17,19}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, kWorkingImageContract))
            throw std::runtime_error("Could not allocate graph input");
        auto ref = result->ref();
        submit([&](VkCommandBuffer cmd) {
            transition(cmd, ref, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue clear{}; std::memcpy(clear.float32, pixel.data(), sizeof(clear.float32));
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd, ref.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            ref.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            transition(cmd, ref, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        result->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return result;
    }
    std::vector<float> download(VulkanImageRef ref) {
        if (ref.ready.valid()) {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1; wait.pSemaphores = &ref.ready.semaphore; wait.pValues = &ref.ready.value;
            check(vkWaitSemaphores(context.device, &wait, UINT64_MAX));
        }
        const size_t count = size_t(ref.extent.width)*ref.extent.height*4;
        VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = count*sizeof(uint16_t); info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(context.device, &info, nullptr, &buffer));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(context.device, buffer, &req);
        VkPhysicalDeviceMemoryProperties props{}; vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &props);
        uint32_t type = UINT32_MAX;
        for (uint32_t i=0;i<props.memoryTypeCount;++i)
            if ((req.memoryTypeBits & (1u<<i)) && (props.memoryTypes[i].propertyFlags
                & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
        if (type == UINT32_MAX) throw std::runtime_error("No readback memory type");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
        check(vkAllocateMemory(context.device, &alloc, nullptr, &memory));
        check(vkBindBufferMemory(context.device, buffer, memory, 0));
        submit([&](VkCommandBuffer cmd) {
            transition(cmd, ref, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent = {ref.extent.width,ref.extent.height,1};
            vkCmdCopyImageToBuffer(cmd, ref.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            auto restore = ref; restore.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            transition(cmd, restore, ref.layout);
        });
        void* mapped = nullptr; check(vkMapMemory(context.device,memory,0,info.size,0,&mapped));
        std::vector<float> result(count);
        for (size_t i=0;i<count;++i) result[i]=halfFloat(static_cast<uint16_t*>(mapped)[i]);
        vkUnmapMemory(context.device,memory); vkDestroyBuffer(context.device,buffer,nullptr);
        vkFreeMemory(context.device,memory,nullptr);
        return result;
    }
    static void expectPixels(const std::vector<float>& actual, std::array<float,4> expected, float tolerance=0.003f) {
        for (size_t i=0;i<actual.size();++i) EXPECT_NEAR(actual[i],expected[i%4],tolerance) << i;
    }
};

TEST(GpuProgramGraphDocumentTest, ValidatesMixedDomainEdgesAndFixedEndpoints) {
    auto document = VulkanGraphDocument::emptyStage();
    const auto encode = document.addProgramNode(gainProgram(), kWorkingImageContract, cctContract);
    const auto contrast = document.addNode("grade_contrast_acescct", {{"contrast",1.4f}}, {}, cctContract, cctContract);
    const auto decode = document.addProgramNode(gainProgram(), cctContract, kWorkingImageContract);
    ASSERT_NE(encode,0u); ASSERT_NE(contrast,0u); ASSERT_NE(decode,0u);
    ASSERT_TRUE(document.connect({1,0},{encode,0}));
    ASSERT_TRUE(document.connect({encode,0},{contrast,0}));
    ASSERT_TRUE(document.connect({contrast,0},{decode,0}));
    ASSERT_TRUE(document.connect({decode,0},{2,0}));
    std::string error; EXPECT_TRUE(document.validate(&error)) << error;
    auto mismatch = VulkanGraphDocument::emptyStage();
    const auto wrong = mismatch.addProgramNode(gainProgram(), cctContract, kWorkingImageContract);
    ASSERT_TRUE(mismatch.connect({1,0},{wrong,0})); ASSERT_TRUE(mismatch.connect({wrong,0},{2,0}));
    EXPECT_FALSE(mismatch.validate(&error)); EXPECT_NE(error.find("contracts"),std::string::npos);
}

TEST_F(GpuProgramGraphTest, DynamicUniformSnapshotsAndReplacementInputsKeepResources) {
    auto document = VulkanGraphDocument::emptyStage();
    const auto id = document.addProgramNode(gainProgram(),kWorkingImageContract,kWorkingImageContract);
    ASSERT_TRUE(document.connect({1,0},{id,0})); ASSERT_TRUE(document.connect({id,0},{2,0}));
    auto source = image({-0.125f,0.25f,2.0f,0.75f});
    auto replacement = image({0.5f,-0.25f,1.0f,0.5f});
    VulkanFilterGraph graph(context,document);
    ASSERT_TRUE(graph.input()->setVulkanInput(source->ref()));
    IBaseNode* node = graph.node(id); VkImage firstOutput = VK_NULL_HANDLE;
    for (const float gain : {1.0f,2.0f,0.5f}) {
        auto program = gainProgram(gain);
        program->uniformUpdate = [data=program->uniformData] { return data; };
        ASSERT_TRUE(graph.setProgram(id,program)); ASSERT_TRUE(graph.graph()->run({}));
        EXPECT_EQ(graph.node(id),node);
        VulkanImageRef output; ASSERT_TRUE(graph.output()->getVulkanOutput(output));
        if (!firstOutput) firstOutput=output.image; EXPECT_EQ(output.image,firstOutput);
        expectPixels(download(output),{-0.125f*gain,0.25f*gain,2.0f*gain,0.75f});
    }
    auto invalidUniforms = gainProgram();
    invalidUniforms->uniformUpdate = [] { return std::vector<std::byte>(8); };
    ASSERT_TRUE(graph.setProgram(id,invalidUniforms));
    EXPECT_THROW(graph.graph()->run({}),std::invalid_argument);
    ASSERT_TRUE(graph.setProgram(id,gainProgram(0.5f)));
    ASSERT_TRUE(graph.graph()->run({}));
    auto changed = gainProgram(); changed->cacheId += "new"; EXPECT_FALSE(graph.setProgram(id,changed));
    EXPECT_FALSE(graph.setProgram(1,gainProgram()));
    ASSERT_TRUE(graph.input()->setVulkanInput(replacement->ref())); ASSERT_TRUE(graph.graph()->run({}));
    VulkanImageRef output; ASSERT_TRUE(graph.output()->getVulkanOutput(output));
    expectPixels(download(output),{0.25f,-0.125f,0.5f,0.5f});
}

TEST_F(GpuProgramGraphTest, MixedConversionAndManifestNodesMatchCpuReference) {
    color::ColorPipeline pipeline;
    auto document = VulkanGraphDocument::emptyStage();
    const auto encode = document.addProgramNode(pipeline.processor("ACEScg","ACEScct").gpuProgramSnapshot(),kWorkingImageContract,cctContract);
    const auto contrast = document.addNode("grade_contrast_acescct",{{"contrast",1.4f}},{},cctContract,cctContract);
    const auto decode = document.addProgramNode(pipeline.processor("ACEScct","ACEScg").gpuProgramSnapshot(),cctContract,kWorkingImageContract);
    ASSERT_TRUE(document.connect({1,0},{encode,0})); ASSERT_TRUE(document.connect({encode,0},{contrast,0}));
    ASSERT_TRUE(document.connect({contrast,0},{decode,0})); ASSERT_TRUE(document.connect({decode,0},{2,0}));
    auto source=image({0.125f,0.25f,0.5f,0.75f});
    VulkanFilterGraph graph(context,document); ASSERT_TRUE(graph.input()->setVulkanInput(source->ref()));
    std::array<float,4> expected{0.125f,0.25f,0.5f,0.75f};
    pipeline.processor("ACEScg","ACEScct").applyRGBA(expected.data(),1);
    for (int c=0;c<3;++c) expected[c]=(expected[c]-0.4135884024924423f)*1.4f+0.4135884024924423f;
    pipeline.processor("ACEScct","ACEScg").applyRGBA(expected.data(),1);
    ASSERT_TRUE(graph.graph()->run({})); VulkanImageRef output;
    ASSERT_TRUE(graph.output()->getVulkanOutput(output)); EXPECT_EQ(output.contract,kWorkingImageContract);
    expectPixels(download(output),expected,0.006f);
}

TEST_F(GpuProgramGraphTest, Float2DAnd3DLutsUseSuppliedBindingsAndSurviveWarmFrames) {
    auto program=gainProgram(); program->cacheId="graph-test-textures";
    program->shaderText = "layout(std140,set=1,binding=0) uniform Params {float gainValue;};"
        "layout(set=1,binding=3) uniform sampler2D shaper;"
        "layout(set=1,binding=7) uniform sampler3D cube;"
        "vec4 gain(vec4 c){return vec4((texture(shaper,vec2(0.5)).rgb+texture(cube,vec3(0.5)).rgb)*gainValue,0.0);}";
    GpuLut two; two.binding=3; two.width=3; two.height=2; two.values.resize(18);
    for (size_t i=0;i<two.values.size();++i) two.values[i]=std::array{-0.125f,0.5f,2.0f}[i%3];
    GpuLut three; three.binding=7; three.width=three.height=three.depth=3; three.is3D=true; three.values.resize(81);
    for (size_t i=0;i<three.values.size();++i) three.values[i]=std::array{0.25f,0.125f,-0.5f}[i%3];
    program->luts={two,three};
    program->sharedLuts = std::make_shared<const std::vector<GpuLut>>(std::move(program->luts));
    auto document=VulkanGraphDocument::emptyStage();
    const auto id=document.addProgramNode(program,kWorkingImageContract,kWorkingImageContract);
    ASSERT_TRUE(document.connect({1,0},{id,0})); ASSERT_TRUE(document.connect({id,0},{2,0}));
    auto source=image({0.1f,0.2f,0.3f,0.75f}); VulkanFilterGraph graph(context,document);
    ASSERT_TRUE(graph.input()->setVulkanInput(source->ref()));
    for (const float gain : {1.0f,2.0f,0.5f}) {
        auto update=std::make_shared<GpuProgram>(*program);
        EXPECT_EQ(update->sharedLuts, program->sharedLuts);
        EXPECT_EQ(update->textures().front().values.data(), program->textures().front().values.data());
        std::memcpy(update->uniformData.data(),&gain,sizeof(float)); ASSERT_TRUE(graph.setProgram(id,update));
        ASSERT_TRUE(graph.graph()->run({})); VulkanImageRef output;
        ASSERT_TRUE(graph.output()->getVulkanOutput(output));
        expectPixels(download(output),{0.125f*gain,0.625f*gain,1.5f*gain,0.75f});
    }
}
} // namespace
