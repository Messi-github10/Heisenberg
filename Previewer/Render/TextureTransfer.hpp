//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

struct AVFrame;
struct pl_frame;

extern "C" {
#include <libplacebo/gpu.h>
}

namespace heisenberg {
namespace render {

class TextureTransfer {
public:
    explicit TextureTransfer(pl_gpu gpu);
    ~TextureTransfer();

    TextureTransfer(const TextureTransfer&) = delete;
    TextureTransfer& operator=(const TextureTransfer&) = delete;

    const pl_frame* uploadAVFrame(const AVFrame* avframe);

private:
    void releaseTextures();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
