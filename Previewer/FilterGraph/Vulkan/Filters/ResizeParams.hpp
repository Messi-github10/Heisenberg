#pragma once

#include <cstdint>

namespace heisenberg::filtergraph {

struct ResizeParams {
    int32_t width = 1920;
    int32_t height = 1080;

    constexpr bool operator==(const ResizeParams&) const = default;
};

} // namespace heisenberg::filtergraph
