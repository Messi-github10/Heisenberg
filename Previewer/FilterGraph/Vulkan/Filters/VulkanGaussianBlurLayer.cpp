#include "VulkanGaussianBlurLayer.hpp"

#include "../VulkanComputeLayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#ifndef HEISENBERG_GAUSSIAN_BLUR_SHADER_PATH
#error HEISENBERG_GAUSSIAN_BLUR_SHADER_PATH must be defined by CMake
#endif

namespace heisenberg::filtergraph {
namespace {

constexpr int32_t kMaximumBlurRadius = 32;

struct alignas(16) GaussianBlurPassUniform {
    int32_t directionX = 0;
    int32_t directionY = 0;
    int32_t radius = 0;
    float sigma = 1.0f;
};

static_assert(sizeof(GaussianBlurPassUniform) == 16);

float automaticSigma(int32_t radius) {
    return std::max(0.1f, (static_cast<float>(radius) - 1.0f) * 0.3f + 0.8f);
}

} // namespace

class VulkanGaussianBlurPassLayer final : public VulkanComputeLayer {
public:
    VulkanGaussianBlurPassLayer(
        std::string mark, int32_t directionX, int32_t directionY)
        : VulkanComputeLayer(std::move(mark)) {
        uniform_.directionX = directionX;
        uniform_.directionY = directionY;
        setUniformBufferSize(sizeof(uniform_));
        updateUniformData(uniform_);
    }

    void updateBlur(int32_t radius, float sigma) {
        uniform_.radius = radius;
        uniform_.sigma = sigma;
        updateUniformData(uniform_);
    }

protected:
    VulkanInputBinding inputBinding(int32_t) const override {
        return VulkanInputBinding::sampledLinear;
    }

    const char* shaderPath() const override {
        return HEISENBERG_GAUSSIAN_BLUR_SHADER_PATH;
    }

private:
    GaussianBlurPassUniform uniform_ = {};
};

VulkanGaussianBlurLayer::VulkanGaussianBlurLayer()
    : VulkanGroupLayer("VulkanGaussianBlur") {
    auto horizontal = std::make_unique<VulkanGaussianBlurPassLayer>(
        "VulkanGaussianBlurHorizontal", 1, 0);
    horizontalPass_ = horizontal.get();
    addPass(std::move(horizontal));

    auto vertical = std::make_unique<VulkanGaussianBlurPassLayer>(
        "VulkanGaussianBlurVertical", 0, 1);
    verticalPass_ = vertical.get();
    addPass(std::move(vertical));

    paramet = {};
    oldParamet = paramet;
    updatePassParameters();
}

VulkanGaussianBlurLayer::~VulkanGaussianBlurLayer() = default;

void VulkanGaussianBlurLayer::onUpdateParamet() {
    if (paramet == oldParamet) return;
    paramet.blurRadius = std::clamp(
        paramet.blurRadius, 0, kMaximumBlurRadius);
    updatePassParameters();
}

void VulkanGaussianBlurLayer::updatePassParameters() {
    const float sigma = paramet.sigma > 0.0f
        ? paramet.sigma : automaticSigma(paramet.blurRadius);
    horizontalPass_->updateBlur(paramet.blurRadius, sigma);
    verticalPass_->updateBlur(paramet.blurRadius, sigma);
}

} // namespace heisenberg::filtergraph
