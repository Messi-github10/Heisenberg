//
// Created by NiceFold on 2026/6/30.
//

#include "RenderEngine.hpp"
#include <libplacebo/renderer.h>
#include <stdexcept>

namespace heisenberg {
namespace renderer {

struct RenderEngine::Impl {
    pl_renderer renderer = nullptr;
};

RenderEngine::RenderEngine(pl_gpu gpu)
    : impl_(std::make_unique<Impl>()) {
    impl_->renderer = pl_renderer_create(nullptr, gpu);
    if (!impl_->renderer) {
        throw std::runtime_error("pl_renderer_create() failed");
    }
}

RenderEngine::~RenderEngine() {
    if (impl_->renderer) {
        pl_renderer_destroy(&impl_->renderer);
    }
}

bool RenderEngine::render(const pl_frame* image, const pl_frame* target,
                          const pl_render_params* params) {
    if (!params) {
        params = &pl_render_default_params;
    }
    return pl_render_image(impl_->renderer, image, target, params);
}

} // namespace renderer
} // namespace heisenberg
