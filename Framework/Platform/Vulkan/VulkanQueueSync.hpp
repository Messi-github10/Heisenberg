#pragma once

#include <mutex>
#include <volk.h>

namespace heisenberg::renderer {
// Vulkan queue calls require external host synchronization. The same lock is
// also supplied to every libplacebo import, so independent preview/export GPU
// contexts cannot submit concurrently to the shared VkQueue. Do not hold this
// lock around pl_gpu_finish/renderer calls: placebo may submit on another thread.
inline std::recursive_mutex& vulkanQueueMutex(VkQueue = VK_NULL_HANDLE) {
    static std::recursive_mutex mutex;
    return mutex;
}
inline void lockVulkanQueue(void*, uint32_t, uint32_t) {
    vulkanQueueMutex().lock();
}
inline void unlockVulkanQueue(void*, uint32_t, uint32_t) {
    vulkanQueueMutex().unlock();
}
inline VkResult synchronizedQueueSubmit(VkQueue queue, uint32_t count,
                                        const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard lock(vulkanQueueMutex(queue));
    return vkQueueSubmit(queue, count, submits, fence);
}
inline VkResult synchronizedQueueWaitIdle(VkQueue queue) {
    std::lock_guard lock(vulkanQueueMutex(queue));
    return vkQueueWaitIdle(queue);
}
inline VkResult synchronizedDeviceWaitIdle(VkDevice device) {
    std::lock_guard lock(vulkanQueueMutex());
    return vkDeviceWaitIdle(device);
}
} // namespace heisenberg::renderer
