#include "VulkanOutputAdapter.hpp"

#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg::filtergraph {

VulkanOutputAdapter::VulkanOutputAdapter()
    : VulkanNode("VulkanOutput", 1, 1) {}

void VulkanOutputAdapter::setObserver(IOutputNodeObserver* observer) {
    observer_ = observer;
}

bool VulkanOutputAdapter::configure(const std::vector<ImageFormat>& inputs) {
    if (inputs.size() != 1
        || inputs[0].format == toFormatId(ImageType::none)) return false;
    setInputFormat(0, inputs[0]);
    setOutputFormat(0, inputs[0]);
    if (observer_) observer_->onFormatChanged(inputs[0], 0);
    return true;
}

bool VulkanOutputAdapter::prepare(const VulkanGraphContext&) {
    return true;
}

bool VulkanOutputAdapter::beginFrame(const FrameContext& frame) {
    if (!input(0).valid()) return false;
    setOutput(0, input(0));
    return VulkanNode::beginFrame(frame);
}

void VulkanOutputAdapter::record(VkCommandBuffer, const FrameContext&) {}

bool VulkanOutputAdapter::getVulkanOutput(
    VulkanImageRef& image, int32_t outputIndex) const {
    if (outputIndex != 0 || !output(0).valid()) return false;
    image = output(0);
    return true;
}

void VulkanOutputAdapter::releaseVulkanOutput(
    const VulkanImageRef& image, int32_t outputIndex) {
    if (outputIndex != 0 || !image.valid() || image.image != output(0).image
        || image.generation != output(0).generation) {
        LOG_WARN("FilterGraph: ignored release for an unknown output image");
        return;
    }
    consumerDone_ = image.ready;
}

VulkanSyncPoint VulkanOutputAdapter::takeConsumerDone() {
    const VulkanSyncPoint result = consumerDone_;
    consumerDone_ = {};
    return result;
}

} // namespace heisenberg::filtergraph
