//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

#include <Common/NonCopy.hpp>

struct AVFrame;
struct pl_frame;

extern "C" {
#include <libplacebo/gpu.h>
}

namespace heisenberg {
namespace renderer {

class TextureTransfer : public NonCopy {
public:
    explicit TextureTransfer(pl_gpu gpu);
    ~TextureTransfer();

    const pl_frame* uploadAVFrame(const AVFrame* avframe);

private:
    void releaseTextures();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
