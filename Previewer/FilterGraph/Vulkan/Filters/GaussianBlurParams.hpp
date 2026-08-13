#pragma once

#include <cstdint>

namespace heisenberg::filtergraph {

struct GaussianBlurParams {
    int32_t blurRadius = 4;
    float sigma = 0.0f;

    bool operator==(const GaussianBlurParams&) const = default;
};

} // namespace heisenberg::filtergraph
