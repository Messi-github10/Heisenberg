//
// Created by NiceFold on 2026/7/14.
//

#pragma once

#include <memory>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
}

struct AVFrame;

namespace heisenberg {
namespace renderer {

class TextureManager : public NonCopy {
public:
    explicit TextureManager(pl_gpu gpu);
    ~TextureManager();

    TextureManager(TextureManager&&)            = delete;
    TextureManager& operator=(TextureManager&&) = delete;

    /// 将 FFmpeg AVFrame 上传为 GPU pl_frame。
    /// @return 内部 pl_frame 指针，下次 uploadAvFrame 调用后可能失效
    const pl_frame* uploadAvFrame(const AVFrame* avframe);

    /// 释放所有 GPU 资源
    void shutdown();

private:
    void releaseUploadTextures();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
