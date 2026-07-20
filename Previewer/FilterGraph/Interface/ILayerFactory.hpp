//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include "ILayer.hpp"

namespace heisenberg {
namespace filtergraph {

// ============================================================
// IOutputLayerObserver — 输出层的 CPU 数据回调
// ============================================================

class IOutputLayerObserver {
public:
    virtual ~IOutputLayerObserver() = default;

    virtual void onImageProcess(uint8_t* data,
                                const ImageFormat& format,
                                int32_t outIndex) = 0;

    virtual void onFormatChanged(const ImageFormat& format,
                                 int32_t outIndex) {}
};

// ============================================================
// IInputLayer — 数据入口
// ============================================================

class IInputLayer : virtual public ILayer {
public:
    ~IInputLayer() override = default;

    /// 设定输入图像格式
    virtual void setImage(const ImageFormat& format) = 0;
    virtual void setImage(const VideoFormat& format) = 0;

    /// 输入 CPU 数据
    virtual void inputCpuData(uint8_t* data, bool bSeparateRun = false) = 0;
    virtual void inputCpuData(const VideoFrame& videoFrame,
                              bool bSeparateRun = false) = 0;
    virtual void inputCpuData(uint8_t* data,
                              const ImageFormat& format,
                              bool bSeparateRun = false) = 0;

    /// 输入 GPU 数据（外部 VkImage 等）
    virtual void inputGpuData(void* device, void* tex) = 0;
};

// ============================================================
// IOutputLayer — 数据出口
// ============================================================

class IOutputLayer : virtual public ILayer {
public:
    ~IOutputLayer() override = default;

    virtual void setObserver(IOutputLayerObserver* observer) = 0;

    /// 输出 Vulkan GPU 纹理
    virtual void outVkGpuTex(const VkOutGpuTex& outTex,
                             int32_t outIndex = 0) = 0;
};

// ============================================================
// LayerFactory — 滤镜工厂（内置滤镜创建）
// ============================================================

class LayerFactory {
public:
    virtual ~LayerFactory() = default;

    virtual IInputLayer*  createInput()      = 0;
    virtual IOutputLayer* createOutput()     = 0;
    virtual ILayer*       createYUV2RGBA()   = 0;
    virtual ILayer*       createRGBA2YUV()   = 0;
    virtual ILayer*       createMapChannel() = 0;
    virtual ILayer*       createFlip()       = 0;
    virtual ILayer*       createResize()     = 0;
    virtual ILayer*       createBlend()      = 0;
};

} // namespace filtergraph
} // namespace heisenberg
