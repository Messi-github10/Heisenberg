#pragma once

#include "INode.hpp"
#include "GaussianBlurParams.hpp"
#include "ResizeParams.hpp"

namespace heisenberg::filtergraph {

class IOutputNodeObserver {
public:
    virtual ~IOutputNodeObserver() = default;

    virtual void onImageProcess(uint8_t* data,
                                const ImageFormat& format,
                                int32_t outIndex) = 0;
    virtual void onFormatChanged(const ImageFormat& format,
                                 int32_t outIndex) {}
};

class IInputNode : virtual public IFilterNode {
public:
    ~IInputNode() override = default;

    virtual void setImage(const ImageFormat& format) = 0;
    virtual void setImage(const VideoFormat& format) = 0;
    virtual bool setVulkanInput(const VulkanImageRef& image,
                                int32_t inputIndex = 0) = 0;
};

class IOutputNode : virtual public IFilterNode {
public:
    ~IOutputNode() override = default;

    virtual void setObserver(IOutputNodeObserver* observer) = 0;
    virtual bool getVulkanOutput(VulkanImageRef& image,
                                 int32_t outputIndex = 0) const = 0;
    virtual void releaseVulkanOutput(const VulkanImageRef& image,
                                     int32_t outputIndex = 0) = 0;
};

class NodeFactory {
public:
    virtual ~NodeFactory() = default;

    virtual IInputNode* createInput() = 0;
    virtual IOutputNode* createOutput() = 0;
    virtual IFilterNode* createPassthrough() = 0;
    virtual IFilterNode* createColorInvert() = 0;
    virtual ITNode<float>* createExposure() = 0;
    virtual ITNode<float>* createBlend() = 0;
    virtual ITNode<GaussianBlurParams>* createGaussianBlur() = 0;
    virtual ITNode<ResizeParams>* createResize() = 0;
    virtual IFilterNode* createLut() = 0;
    virtual IFilterNode* createHistogram() = 0;
};

} // namespace heisenberg::filtergraph
