//
// Created by NiceFold on 2026/6/30.
//

#include "RenderEngine.hpp"

namespace heisenberg {
namespace render {

struct RenderEngine::Impl {};

RenderEngine::RenderEngine()  : impl_(std::make_unique<Impl>()) {}
RenderEngine::~RenderEngine() = default;

} // namespace render
} // namespace heisenberg
