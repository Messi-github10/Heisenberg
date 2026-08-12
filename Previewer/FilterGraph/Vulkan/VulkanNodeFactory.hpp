#pragma once

#include <FilterGraph/Interface/ILayerFactory.hpp>
#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class BaseLayer;

class VulkanNodeFactory final : public LayerFactory {
public:
    VulkanNodeFactory();
    ~VulkanNodeFactory() override;

    IInputLayer* createInput() override;
    IOutputLayer* createOutput() override;
    ILayer* createPassthrough() override;
    ILayer* createColorInvert() override;
    ITLayer<float>* createExposure() override;
    ITLayer<float>* createBlend() override;
    ITLayer<GaussianBlurParamet>* createGaussianBlur() override;

private:
    template<typename T>
    T* createNode();

    std::vector<std::unique_ptr<BaseLayer>> nodes_;
};

} // namespace heisenberg::filtergraph
