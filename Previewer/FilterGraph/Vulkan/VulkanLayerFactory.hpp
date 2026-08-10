#pragma once

#include <FilterGraph/Interface/ILayerFactory.hpp>
#include <FilterGraph/Interface/IPipeGraph.hpp>

#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class BaseLayer;

class VulkanLayerFactory final : public LayerFactory {
public:
    VulkanLayerFactory();
    ~VulkanLayerFactory() override;

    IInputLayer* createInput() override;
    IOutputLayer* createOutput() override;
    ILayer* createPassthrough() override;
    ILayer* createColorInvert() override;
    ITLayer<float>* createExposure() override;
    ITLayer<float>* createBlend() override;
    ITLayer<GaussianBlurParamet>* createGaussianBlur() override;

private:
    template<typename T>
    T* createLayer();

    std::vector<std::unique_ptr<BaseLayer>> layers_;
};

class VulkanPipeGraphFactory final : public PipeGraphFactory {
public:
    IPipeGraph* createGraph(const VulkanGraphContext& context) override;
};

} // namespace heisenberg::filtergraph
