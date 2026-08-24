#pragma once

#include <cstdint>

namespace heisenberg::filtergraph::shader_abi {

// All compute shaders use descriptor set 0. Image bindings are ordered as
// regular inputs, extra inputs, and outputs; the optional UBO follows them.
inline constexpr uint32_t kDescriptorSet = 0;
inline constexpr uint32_t kFirstInputBinding = 0;

constexpr uint32_t inputBinding(uint32_t inputIndex) {
    return kFirstInputBinding + inputIndex;
}

constexpr uint32_t extraInputBinding(uint32_t inputCount,
                                     uint32_t extraInputIndex) {
    return kFirstInputBinding + inputCount + extraInputIndex;
}

constexpr uint32_t outputBinding(uint32_t inputCount,
                                 uint32_t extraInputCount,
                                 uint32_t outputIndex) {
    return kFirstInputBinding + inputCount + extraInputCount + outputIndex;
}

constexpr uint32_t uniformBinding(uint32_t inputCount,
                                  uint32_t extraInputCount,
                                  uint32_t outputCount) {
    return kFirstInputBinding + inputCount + extraInputCount + outputCount;
}

inline constexpr uint32_t kHistogramInputBinding = 0;
inline constexpr uint32_t kHistogramBinsBinding = 1;

} // namespace heisenberg::filtergraph::shader_abi
