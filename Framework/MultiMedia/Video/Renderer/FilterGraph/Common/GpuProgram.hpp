#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace heisenberg::filtergraph {

// Backend-neutral compute function. Set 0: sampled input at binding 0,
// storage output at binding 1. Set 1: optional std140 UBO at binding 0,
// float LUT samplers at their explicitly supplied bindings (starting at 1).
struct GpuLut {
    uint32_t binding = 0;
    uint32_t width = 0, height = 0, depth = 1;
    uint32_t channels = 3;
    bool is3D = false;
    bool linearInterpolation = true;
    std::vector<float> values;
};

struct GpuProgram {
    // Immutable identity of shader, LUT contents/bindings and UBO layout.
    // A new identity requires graph recompilation; uniform values may change.
    std::string cacheId;
    std::string shaderText;
    std::string functionName;
    std::vector<std::byte> uniformData;
    std::vector<GpuLut> luts;
    std::function<std::vector<std::byte>()> uniformUpdate;
    // Shared immutable LUT payload lets numeric snapshots copy only uniforms.
    // When set, this payload takes precedence over the legacy owned luts field.
    std::shared_ptr<const std::vector<GpuLut>> sharedLuts;
    const std::vector<GpuLut>& textures() const {
        return sharedLuts ? *sharedLuts : luts;
    }
};

} // namespace heisenberg::filtergraph
