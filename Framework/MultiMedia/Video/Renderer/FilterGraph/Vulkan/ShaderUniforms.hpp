#pragma once

#include <cstddef>
#include <cstdint>

namespace heisenberg::filtergraph::shader_abi {

// std140 gives a standalone scalar block a 16-byte base alignment. The
// shader only reads the first float, but the C++ allocation must preserve the
// block's alignment and stride.
struct alignas(16) ScalarUniform {
    float value = 0.0f;
    std::byte padding[12]{};
};

static_assert(alignof(ScalarUniform) == 16);
static_assert(offsetof(ScalarUniform, value) == 0);
static_assert(sizeof(ScalarUniform) == 16);

// Matches the Gaussian blur UBO in gaussianBlur.comp under std140.
struct alignas(16) GaussianBlurUniform {
    int32_t directionX = 0;
    int32_t directionY = 0;
    int32_t radius = 0;
    float sigma = 1.0f;
};

static_assert(alignof(GaussianBlurUniform) == 16);
static_assert(offsetof(GaussianBlurUniform, directionX) == 0);
static_assert(offsetof(GaussianBlurUniform, directionY) == 4);
static_assert(offsetof(GaussianBlurUniform, radius) == 8);
static_assert(offsetof(GaussianBlurUniform, sigma) == 12);
static_assert(sizeof(GaussianBlurUniform) == 16);

} // namespace heisenberg::filtergraph::shader_abi
