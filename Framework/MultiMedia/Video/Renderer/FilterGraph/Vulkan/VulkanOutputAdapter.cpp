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
    return input(0).valid() && VulkanNode::beginFrame(frame);
}

void VulkanOutputAdapter::record(VkCommandBuffer, const FrameContext&) {}

bool VulkanOutputAdapter::getVulkanOutput(
    VulkanImageRef& image, int32_t outputIndex) const {
    if (outputIndex != 0 || !input(0).valid()) return false;
    image = input(0);
    return true;
}

void VulkanOutputAdapter::releaseVulkanOutput(
    const VulkanImageRef& image, int32_t outputIndex) {
    const VulkanImageRef current = input(0);
    if (outputIndex != 0 || !image.valid() || image.image != current.image
        || image.generation != current.generation) {
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

void VulkanOutputAdapter::bindDeclaredResources(
    const std::vector<LogicalResourceId>&) {}

LogicalResourceId VulkanOutputAdapter::logicalOutputResource(int32_t index) const {
    return index == 0 ? inputResource(0) : LogicalResourceId{};
}

} // namespace heisenberg::filtergraph
