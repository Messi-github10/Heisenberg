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

    /// Borrow a renderer-owned Vulkan image as this frame's graph input.
    virtual bool setVulkanInput(const VulkanImageRef& image,
                                int32_t inputIndex = 0) = 0;
};

// ============================================================
// IOutputLayer — 数据出口
// ============================================================

class IOutputLayer : virtual public ILayer {
public:
    ~IOutputLayer() override = default;

    virtual void setObserver(IOutputLayerObserver* observer) = 0;

    /// Borrow the graph-owned output image after run() has submitted the frame.
    virtual bool getVulkanOutput(VulkanImageRef& image,
                                 int32_t outputIndex = 0) const = 0;

    /// Return an output image after the external consumer has finished reading it.
    virtual void releaseVulkanOutput(const VulkanImageRef& image,
                                     int32_t outputIndex = 0) = 0;
};

// ============================================================
// LayerFactory — 滤镜工厂（内置滤镜创建）
// ============================================================

class LayerFactory {
public:
    virtual ~LayerFactory() = default;

    virtual IInputLayer*  createInput()      = 0;
    virtual IOutputLayer* createOutput()     = 0;
    virtual ILayer*       createPassthrough() = 0;
    virtual ILayer*       createColorInvert() = 0;
    virtual ITLayer<float>* createExposure() = 0;
    virtual ITLayer<float>* createBlend() = 0;
    virtual ITLayer<GaussianBlurParamet>* createGaussianBlur() = 0;
};

} // namespace filtergraph
} // namespace heisenberg
