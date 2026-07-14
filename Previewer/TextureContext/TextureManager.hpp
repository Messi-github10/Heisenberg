//
// Created by NiceFold on 2026/7/14.
//

#pragma once

#include <memory>
#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
}

struct AVFrame;

namespace heisenberg {
namespace renderer {

class ITexturePool;

class TextureManager : public NonCopy {
public:
    TextureManager(pl_gpu gpu, vk::Device device, uint32_t queueFamily);
    ~TextureManager();

    TextureManager(TextureManager&&)            = delete;
    TextureManager& operator=(TextureManager&&) = delete;

    struct TargetAcquisition {
        pl_tex        tex   = nullptr;
        const pl_frame* frame = nullptr;
    };

    /// 将 FFmpeg AVFrame 上传为 GPU pl_frame。
    /// @return 内部 pl_frame 指针，下次 uploadAvFrame 后失效
    const pl_frame* uploadAvFrame(const AVFrame* avframe);

    /// 获取渲染目标。优先从池中复用 VkImage，否则新建 pl_tex。
    TargetAcquisition acquireTarget(int width, int height);

    /// 渲染成功后：flush GPU → hold (layout 转换) → unwrap pl_tex → 返回 VkImage
    /// @return 可用于 Qt 场景图采样的 VkImage
    vk::Image finalizeAndExport(pl_tex tex, int width, int height);

    /// 渲染失败时清理目标纹理
    void discardTarget(pl_tex tex, int width, int height);

    /// Qt 用完 VkImage 后的回调 — 归还池以供复用
    void recycleImage(vk::Image image);

    /// 销毁所有 GPU 资源
    void shutdown();

private:
    pl_tex createTargetTex(int width, int height);
    pl_tex wrapPoolImage(vk::Image image, int width, int height,
                         vk::ImageLayout currentLayout,
                         VkImageUsageFlags actualUsage);
    void   buildTargetFrame(pl_tex tex, int width, int height);
    void   releaseUploadTextures();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
