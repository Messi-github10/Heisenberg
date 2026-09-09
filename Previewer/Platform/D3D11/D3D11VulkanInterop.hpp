#pragma once

#include <Common/NonCopy.hpp>
#include <Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <vulkan/vulkan.h>

#include <memory>

struct AVFrame;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace heisenberg::renderer {

/// D3D11VA frame -> D3D11 shared RGBA16F -> imported Vulkan image.
/// The exported image follows filtergraph::kWorkingImageContract.
class D3D11VulkanInterop final : public NonCopy {
public:
    D3D11VulkanInterop();
    ~D3D11VulkanInterop();

    bool init(ID3D11Device* device, ID3D11DeviceContext* context,
              VkDevice vkDevice, VkPhysicalDevice vkPhysicalDevice,
              VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
              int width, int height);
    void shutdown();

    bool processFrame(const AVFrame* hwFrame,
                      filtergraph::VulkanImageRef& out);
    void releaseFrame(const filtergraph::VulkanSyncPoint& done);
    bool resize(int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg::renderer
