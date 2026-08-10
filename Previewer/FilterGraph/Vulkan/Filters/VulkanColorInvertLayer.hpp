#pragma once

#include "../VulkanComputeLayer.hpp"

namespace heisenberg::filtergraph {

class VulkanColorInvertLayer final : public VulkanComputeLayer {
public:
    VulkanColorInvertLayer();

protected:
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
