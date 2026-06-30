//
// Created by NiceFold on 2026/6/30.
//

#include "GpuContext.hpp"

namespace heisenberg {
namespace render {

struct GpuContext::Impl {};

GpuContext::GpuContext()  : impl_(std::make_unique<Impl>()) {}
GpuContext::~GpuContext() = default;

} // namespace render
} // namespace heisenberg
