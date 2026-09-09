#pragma once

#include <Common/NonCopy.hpp>

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace heisenberg::renderer {

class D3D11Context final : public NonCopy {
public:
    static D3D11Context& instance();
    ~D3D11Context();

    void createDevice();
    ID3D11Device* device() const;
    ID3D11DeviceContext* context() const;
    bool sharedResourceTier2() const;

private:
    D3D11Context();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg::renderer
