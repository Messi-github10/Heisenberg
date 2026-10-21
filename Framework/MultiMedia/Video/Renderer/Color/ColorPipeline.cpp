#include "ColorPipeline.hpp"
#include <OpenColorIO/OpenColorIO.h>
extern "C" {
#include <libavutil/pixdesc.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <map>
#include <mutex>
#include <utility>

namespace OCIO = OCIO_NAMESPACE;
namespace heisenberg::renderer::color {
namespace {
template<class T> void put(std::vector<std::byte>& bytes, size_t offset, const T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("OCIO uniform offset exceeds its declared buffer size");
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
}
struct ColorProcessor::Impl {
    OCIO::ConstProcessorRcPtr processor;
    OCIO::ConstCPUProcessorRcPtr cpu;
    mutable std::mutex gpuMutex;
    mutable std::shared_ptr<const GpuProgram> gpuSnapshot;
};
ColorProcessor::ColorProcessor() = default;
ColorProcessor::~ColorProcessor() = default;
ColorProcessor::ColorProcessor(const ColorProcessor&) = default;
ColorProcessor& ColorProcessor::operator=(const ColorProcessor&) = default;
ColorProcessor::operator bool() const {
    return bool(impl_);
}
void ColorProcessor::applyRGBA(float* pixels, size_t count) const {
    if (!impl_ || (!pixels && count)) throw std::invalid_argument("Invalid ColorProcessor/pixel buffer");
    // Applying RGB individually explicitly excludes alpha from every transform,
    // including externally supplied LUTs/matrices that might otherwise alter it.
    for (size_t i = 0; i < count; ++i) impl_->cpu->applyRGB(pixels + i * 4);
}
GpuProgram ColorProcessor::gpuProgram() const {
    return *gpuProgramSnapshot();
}
std::shared_ptr<const GpuProgram> ColorProcessor::gpuProgramSnapshot() const {
    if (!impl_) throw std::invalid_argument("Empty ColorProcessor");
    std::lock_guard lock(impl_->gpuMutex);
    if (impl_->gpuSnapshot) return impl_->gpuSnapshot;
    auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    desc->setFunctionName("heisenbergColor");
    desc->setResourcePrefix("heisenberg_ocio_");
    desc->setDescriptorSetIndex(1, 1);
    desc->setAllowTexture1D(false);
    impl_->processor->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);
    GpuProgram result;
    result.cacheId = desc->getCacheID();
    result.shaderText = desc->getShaderText();
    result.functionName = desc->getFunctionName();
    result.uniformData.resize(desc->getUniformBufferSize());
    if (desc->getNumUniforms()) {
        result.uniformUpdate = [desc] {
            std::vector<std::byte> bytes(desc->getUniformBufferSize());
            for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
                OCIO::GpuShaderDesc::UniformData data;
                desc->getUniform(i, data);
                const auto offset = data.m_bufferOffset;
                switch (data.m_type) {
                case OCIO::UNIFORM_DOUBLE: put(bytes, offset, float(data.m_getDouble())); break;
                case OCIO::UNIFORM_BOOL: put(bytes, offset, int32_t(data.m_getBool() ? 1 : 0)); break;
                case OCIO::UNIFORM_FLOAT3: {
                    const auto& value = data.m_getFloat3();
                    for (size_t j = 0; j < 3; ++j) put(bytes, offset + j * 4, value[j]);
                    break;
                }
                case OCIO::UNIFORM_VECTOR_FLOAT: {
                    const int size = data.m_vectorFloat.m_getSize();
                    const float* values = data.m_vectorFloat.m_getVector();
                    if (size < 0 || (size && !values)) throw std::runtime_error("Invalid OCIO uniform array");
                    for (int j = 0; j < size; ++j) put(bytes, offset + size_t(j) * 16, values[j]);
                    break;
                }
                case OCIO::UNIFORM_VECTOR_INT: {
                    const int size = data.m_vectorInt.m_getSize();
                    const int* values = data.m_vectorInt.m_getVector();
                    if (size < 0 || (size && !values)) throw std::runtime_error("Invalid OCIO uniform array");
                    for (int j = 0; j < size; ++j) put(bytes, offset + size_t(j) * 16, int32_t(values[j]));
                    break;
                }
                default: throw std::runtime_error("Unsupported OCIO uniform type");
                }
            }
            return bytes;
        };
        result.uniformData = result.uniformUpdate();
    }
    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char *texture, *sampler;
        OCIO::GpuShaderCreator::TextureType channel;
        OCIO::GpuShaderCreator::TextureDimensions dimensions;
        OCIO::Interpolation interpolation;
        GpuLut lut;
        desc->getTexture(i, texture, sampler, lut.width, lut.height, channel, dimensions, interpolation);
        if (dimensions != OCIO::GpuShaderCreator::TEXTURE_2D)
            throw std::runtime_error("OCIO generated an unexpected 1D texture");
        lut.channels = channel == OCIO::GpuShaderCreator::TEXTURE_RED_CHANNEL ? 1 : 3;
        lut.binding = desc->getTextureShaderBindingIndex(i);
        lut.linearInterpolation = interpolation != OCIO::INTERP_NEAREST;
        const float* values = nullptr;
        desc->getTextureValues(i, values);
        lut.values.assign(values, values + size_t(lut.width) * lut.height * lut.channels);
        result.luts.push_back(std::move(lut));
    }
    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char *texture, *sampler;
        OCIO::Interpolation interpolation;
        GpuLut lut;
        desc->get3DTexture(i, texture, sampler, lut.width, interpolation);
        lut.height = lut.depth = lut.width;
        lut.is3D = true;
        lut.binding = desc->get3DTextureShaderBindingIndex(i);
        // Tetrahedral interpolation is emitted as explicit shader sampling;
        // hardware filtering remains nearest for that path.
        lut.linearInterpolation = interpolation == OCIO::INTERP_LINEAR;
        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        lut.values.assign(values, values + size_t(lut.width) * lut.height * lut.depth * 3);
        result.luts.push_back(std::move(lut));
    }
    impl_->gpuSnapshot = std::make_shared<GpuProgram>(std::move(result));
    return impl_->gpuSnapshot;
}
struct ColorPipeline::Impl {
    OCIO::ConstConfigRcPtr config;
    std::unique_ptr<ColorGpuPass> gpu;
    mutable std::mutex mutex;
    struct CachedProcessor { ColorProcessor processor; uint64_t used = 0; };
    mutable std::map<std::string, CachedProcessor> processors;
    mutable uint64_t clock = 0;
    void store(const std::string& key, const ColorProcessor& processor) const {
        if (processors.size() >= kProgramCacheCapacity) {
            auto oldest = std::min_element(processors.begin(), processors.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            processors.erase(oldest);
        }
        processors.emplace(key, CachedProcessor{processor, ++clock});
    }
    explicit Impl(const std::string& id) {
        if (id != kConfigId) throw std::invalid_argument("Unsupported color config ID: " + id);
        // Deliberately independent of OCIO environment/current global config.
        config = OCIO::Config::CreateFromBuiltinConfig(id.c_str());
        config->validate();
    }
};
ColorPipeline::ColorPipeline() : impl_(std::make_unique<Impl>(kConfigId)) {}
ColorPipeline::ColorPipeline(filtergraph::VulkanGraphContext context, const std::string& id)
    : impl_(std::make_unique<Impl>(id)) { impl_->gpu = std::make_unique<ColorGpuPass>(context); }
ColorPipeline::~ColorPipeline() = default;
void ColorPipeline::setCancellation(std::function<bool()> cancellation) {
    if (impl_->gpu) impl_->gpu->setCancellation(std::move(cancellation));
}
ColorProcessor ColorPipeline::processor(const std::string& source, const std::string& destination) const {
    std::lock_guard lock(impl_->mutex);
    const std::string key = "space\n" + source + "\n" + destination;
    if (auto it = impl_->processors.find(key); it != impl_->processors.end()) {
        it->second.used = ++impl_->clock; return it->second.processor;
    }
    ColorProcessor result;
    result.impl_ = std::make_shared<ColorProcessor::Impl>();
    result.impl_->processor = impl_->config->getProcessor(source.c_str(), destination.c_str());
    result.impl_->cpu = result.impl_->processor->getDefaultCPUProcessor();
    impl_->store(key, result);
    return result;
}
ColorProcessor ColorPipeline::lutProcessor(const std::string& filename) const {
    // File contents may change under the same path; do not cache by filename.
    auto transform = OCIO::FileTransform::Create();
    transform->setSrc(filename.c_str());
    transform->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
    ColorProcessor result;
    result.impl_ = std::make_shared<ColorProcessor::Impl>();
    result.impl_->processor = impl_->config->getProcessor(transform);
    result.impl_->cpu = result.impl_->processor->getDefaultCPUProcessor();
    return result;
}
ColorProcessor ColorPipeline::lutProcessor(uint32_t size,
    const std::vector<std::array<float, 3>>& samples,
    const std::array<float, 3>& domainMin,
    const std::array<float, 3>& domainMax) const {
    if (size < 2 || size > 128 || samples.size() != size_t(size) * size * size)
        throw std::invalid_argument("Invalid 3D LUT size or sample count");
    auto group = OCIO::GroupTransform::Create();
    auto domain = OCIO::MatrixTransform::Create();
    double matrix[16] = {}, offset[4] = {};
    matrix[15] = 1.0;
    for (size_t c = 0; c < 3; ++c) {
        const double span = double(domainMax[c]) - domainMin[c];
        if (!std::isfinite(span) || span <= 0.0 || !std::isfinite(domainMin[c]))
            throw std::invalid_argument("Invalid 3D LUT input domain");
        matrix[c * 5] = 1.0 / span;
        offset[c] = -domainMin[c] / span;
    }
    domain->setMatrix(matrix); domain->setOffset(offset);
    group->appendTransform(domain);
    auto lut = OCIO::Lut3DTransform::Create(size);
    lut->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
    for (uint32_t b = 0; b < size; ++b)
        for (uint32_t g = 0; g < size; ++g)
            for (uint32_t r = 0; r < size; ++r) {
                const auto& value = samples[(size_t(b) * size + g) * size + r];
                if (!std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); }))
                    throw std::invalid_argument("Non-finite 3D LUT sample");
                lut->setValue(r, g, b, value[0], value[1], value[2]);
            }
    group->appendTransform(lut);
    ColorProcessor result;
    result.impl_ = std::make_shared<ColorProcessor::Impl>();
    result.impl_->processor = impl_->config->getProcessor(group);
    result.impl_->cpu = result.impl_->processor->getDefaultCPUProcessor();
    return result;
}
ColorProcessor ColorPipeline::displayProcessor(const std::string& source,
    const std::string& display, const std::string& view) const {
    std::lock_guard lock(impl_->mutex);
    const std::string key = "display\n" + source + "\n" + display + "\n" + view;
    if (auto it = impl_->processors.find(key); it != impl_->processors.end()) {
        it->second.used = ++impl_->clock; return it->second.processor;
    }
    ColorProcessor result;
    result.impl_ = std::make_shared<ColorProcessor::Impl>();
    result.impl_->processor = impl_->config->getProcessor(source.c_str(), display.c_str(), view.c_str(), OCIO::TRANSFORM_DIR_FORWARD);
    result.impl_->cpu = result.impl_->processor->getDefaultCPUProcessor();
    impl_->store(key, result);
    return result;
}
ColorInputResolve ColorPipeline::resolveInput(const VideoSpec& metadata, const std::string& overrideSpace,
    int inputPolicyVersion) const {
    if (inputPolicyVersion != 1 && inputPolicyVersion != 2)
        throw std::invalid_argument("Unsupported input color policy version: " + std::to_string(inputPolicyVersion));
    const bool displayReferred = inputPolicyVersion == 2;
    ColorInputResolve result;
    result.observedMetadata = metadata;
    result.overrideSpace = overrideSpace;
    if (!overrideSpace.empty()) {
        const auto space = impl_->config->getColorSpace(overrideSpace.c_str());
        if (!space) throw std::invalid_argument("Unknown OCIO input override: " + overrideSpace);
        result.colorSpace = space->getName();
        result.source = InputSelectionSource::Override;
        return result;
    }
    const auto primary = metadata.colorPrimaries;
    const auto transfer = metadata.colorTransfer;
    if (primary == AVCOL_PRI_BT709) {
        if (transfer == AVCOL_TRC_IEC61966_2_1) result.colorSpace = displayReferred ? "sRGB - Display" : "sRGB Encoded Rec.709 (sRGB)";
        else if (transfer == AVCOL_TRC_BT709 || transfer == AVCOL_TRC_SMPTE170M) result.colorSpace = displayReferred ? "Rec.1886 Rec.709 - Display" : "Camera Rec.709";
        else if (transfer == AVCOL_TRC_LINEAR) result.colorSpace = "Linear Rec.709 (sRGB)";
        else if (transfer == AVCOL_TRC_GAMMA22) result.colorSpace = displayReferred ? "Gamma 2.2 Rec.709 - Display" : "Gamma 2.2 Encoded Rec.709";
    } else if (primary == AVCOL_PRI_BT2020) {
        if (transfer == AVCOL_TRC_LINEAR) result.colorSpace = "Linear Rec.2020";
        else if (transfer == AVCOL_TRC_SMPTE2084) result.colorSpace = "Rec.2100-PQ - Display";
        else if (transfer == AVCOL_TRC_ARIB_STD_B67) result.colorSpace = "Rec.2100-HLG - Display";
    } else if (primary == AVCOL_PRI_SMPTE432) {
        if (transfer == AVCOL_TRC_LINEAR) result.colorSpace = "Linear P3-D65";
        else if (transfer == AVCOL_TRC_IEC61966_2_1) result.colorSpace = "sRGB Encoded P3-D65";
    }
    if (!result.colorSpace.empty()) {
        result.source = InputSelectionSource::Metadata;
    } else {
        const auto* desc = av_pix_fmt_desc_get(metadata.format);
        const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
        result.colorSpace = displayReferred
            ? (rgb ? "sRGB - Display" : "Rec.1886 Rec.709 - Display")
            : (rgb ? "sRGB Encoded Rec.709 (sRGB)" : "Camera Rec.709");
        result.source = InputSelectionSource::Fallback;
        result.diagnostics.push_back("Unsupported or absent source primaries/transfer (" + std::to_string(int(primary)) + "," + std::to_string(int(transfer)) + "); explicit deterministic " + (rgb ? "RGB sRGB" : "video Rec.709") + " fallback; supply an input override for camera/log media");
    }
    if (metadata.colorRange == AVCOL_RANGE_UNSPECIFIED)
        result.diagnostics.push_back("Source range unspecified; input adapter must report its range fallback independently");
    return result;
}
filtergraph::VulkanSyncPoint ColorPipeline::executeSpace(const filtergraph::VulkanImageRef& input,
    const filtergraph::VulkanImageRef& output, const std::string& source,
    const std::string& destination, std::string* error) {
    try {
        if (!impl_->gpu) throw std::runtime_error("ColorPipeline has no Vulkan context; CPU is not a GPU fallback");
        auto ready = impl_->gpu->execute(*processor(source, destination).gpuProgramSnapshot(), input, output);
        if (error) error->clear();
        return ready;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return {}; }
}
filtergraph::VulkanSyncPoint ColorPipeline::executeDisplay(const filtergraph::VulkanImageRef& input,
    const filtergraph::VulkanImageRef& output, const std::string& source,
    const std::string& display, const std::string& view, std::string* error) {
    try {
        if (!impl_->gpu) throw std::runtime_error("ColorPipeline has no Vulkan context; CPU is not a GPU fallback");
        auto ready = impl_->gpu->execute(*displayProcessor(source, display, view).gpuProgramSnapshot(), input, output);
        if (error) error->clear();
        return ready;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return {}; }
}
std::vector<std::string> ColorPipeline::colorSpaces() const {
    std::vector<std::string> result;
    for (int i = 0; i < impl_->config->getNumColorSpaces(); ++i)
        result.emplace_back(impl_->config->getColorSpaceNameByIndex(i));
    return result;
}
} // namespace heisenberg::renderer::color
