#include "D3D11Context.hpp"

#include <Platform/Vulkan/VulkanContext.hpp>

#include <Utiles/Logger.hpp>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d10.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <cstring>

namespace heisenberg::renderer {

struct D3D11Context::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool tier2 = false;
    bool created = false;

    void release() {
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
        tier2 = false;
        created = false;
    }
};

D3D11Context& D3D11Context::instance() {
    static D3D11Context context;
    return context;
}

D3D11Context::D3D11Context() : impl_(std::make_unique<Impl>()) {}

D3D11Context::~D3D11Context() {
    impl_->release();
}

void D3D11Context::createDevice() {
    if (impl_->created) return;
    impl_->release();

    // Do not enable the D3D11 SDK debug layer by default.  FFmpeg's D3D11VA
    // backend uses the externally supplied device from a worker thread, and
    // some Windows/SDK-layer combinations can raise an access violation in
    // D3D11SDKLayers.dll while av_hwdevice_ctx_init() configures that device.
    // Opt in explicitly when diagnosing D3D11 calls.
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    wchar_t debugLayer[2]{};
    if (GetEnvironmentVariableW(L"HEISENBERG_D3D11_DEBUG", debugLayer,
                                ARRAYSIZE(debugLayer)) > 0
        && debugLayer[0] == L'1') {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory1* factory = nullptr;
    if (VulkanContext::instance().deviceLuidValid()
        && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        const auto& vulkanLuid = VulkanContext::instance().deviceLuid();
        LUID targetLuid{};
        static_assert(sizeof(targetLuid) == VK_LUID_SIZE);
        std::memcpy(&targetLuid, vulkanLuid.data(), sizeof(targetLuid));
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1* candidate = nullptr;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!candidate) continue;
            DXGI_ADAPTER_DESC1 desc{};
            const bool matches = SUCCEEDED(candidate->GetDesc1(&desc))
                && desc.AdapterLuid.LowPart == targetLuid.LowPart
                && desc.AdapterLuid.HighPart == targetLuid.HighPart;
            if (matches) {
                adapter = candidate;
                break;
            }
            candidate->Release();
        }
        factory->Release();
        if (!adapter) {
            LOG_WARN("D3D11Context: no DXGI adapter matched the Vulkan LUID; "
                     "using the default hardware adapter");
        }
    }
    D3D_FEATURE_LEVEL selected{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT result = D3D11CreateDevice(
        adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, &selected, &context);
    if (adapter) adapter->Release();
    if (FAILED(result) || !device || !context) {
        if (context) context->Release();
        if (device) device->Release();
        LOG_ERROR("D3D11Context: D3D11CreateDevice failed: 0x{:08x}",
                  static_cast<unsigned>(result));
        return;
    }
    impl_->device = device;
    impl_->context = context;
    impl_->created = true;

    // FFmpeg's D3D11VA decoder runs on the decode thread while libplacebo
    // performs the color conversion on the presentation thread. Protect the
    // shared immediate context so both users can safely issue commands.
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }

    ID3D11Device5* device5 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS5 options{};
        if (SUCCEEDED(device5->CheckFeatureSupport(
                D3D11_FEATURE_D3D11_OPTIONS5, &options, sizeof(options)))) {
            impl_->tier2 = options.SharedResourceTier >=
                           D3D11_SHARED_RESOURCE_TIER_2;
        }
        device5->Release();
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)) &&
                VulkanContext::instance().deviceLuidValid()) {
                const auto& luid = VulkanContext::instance().deviceLuid();
                LUID vulkanLuid{};
                static_assert(sizeof(vulkanLuid) == VK_LUID_SIZE);
                std::memcpy(&vulkanLuid, luid.data(), sizeof(vulkanLuid));
                if (desc.AdapterLuid.LowPart != vulkanLuid.LowPart ||
                    desc.AdapterLuid.HighPart != vulkanLuid.HighPart) {
                    LOG_WARN("D3D11Context: adapter LUID differs from Vulkan device; "
                             "external sharing may fail");
                }
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    LOG_INFO("D3D11Context: device created, feature level 0x{:04x}, "
             "sharedResourceTier2={}", static_cast<unsigned>(selected),
             impl_->tier2);
}

ID3D11Device* D3D11Context::device() const { return impl_->device; }
ID3D11DeviceContext* D3D11Context::context() const { return impl_->context; }
bool D3D11Context::sharedResourceTier2() const { return impl_->tier2; }

} // namespace heisenberg::renderer
