//
// Created by NiceFold on 2026/6/30.
//

#include "SwapChain.hpp"

namespace heisenberg {
namespace render {

struct SwapChain::Impl {};

SwapChain::SwapChain()  : impl_(std::make_unique<Impl>()) {}
SwapChain::~SwapChain() = default;

} // namespace render
} // namespace heisenberg
