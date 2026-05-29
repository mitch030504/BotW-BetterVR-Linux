#pragma once
#include "openxr.h"
#include "texture.h"

// Set in the vkCreateDevice hook when we successfully request extra queues
// from Cemu's graphics family. Read by VRManager::Init when constructing
// RND_Vulkan (which happens later, after the device exists).
//
// Three-queue topology:
//   queue 0 → Cemu's main queue (unchanged)
//   queue 1 (= g_requestedSubmitQueueIndex) → OpenXR runtime
//   queue 2 (= g_requestedCopyQueueIndex)   → our submit-thread copies
//
// This separation prevents queue saturation: runtime composition and our
// intermediate→swapchain copies don't pile up behind each other.
// Linux-only: the three-queue topology exists only for the Vulkan submit-thread
// backend; the Windows/D3D12 path never requests or uses these queues.
#ifndef _WIN32
extern std::atomic<int32_t> g_requestedSubmitQueueIndex;
extern std::atomic<int32_t> g_requestedCopyQueueIndex;
#endif

class RND_Vulkan {
public:
    RND_Vulkan(VkInstance vkInstance, VkPhysicalDevice vkPhysDevice, VkDevice vkDevice);
    ~RND_Vulkan();

    uint32_t FindMemoryType(uint32_t memoryTypeBitsRequirement, VkMemoryPropertyFlags requirementsMask);
    VkInstance GetInstance() { return m_instance; }
    VkDevice GetDevice() { return m_device; }
    VkPhysicalDevice GetPhysicalDevice() { return m_physicalDevice; }

    const vkroots::VkInstanceDispatch* GetInstanceDispatch() const { return m_instanceDispatch; }
    const vkroots::VkPhysicalDeviceDispatch* GetPhysicalDeviceDispatch() const { return m_physicalDeviceDispatch; }
    const vkroots::VkDeviceDispatch* GetDeviceDispatch() const { return m_deviceDispatch; }

private:
    VkInstance m_instance;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VkPhysicalDeviceMemoryProperties2 m_memoryProperties = {};
    // todo: use these with caution
    const vkroots::VkInstanceDispatch* m_instanceDispatch;
    const vkroots::VkPhysicalDeviceDispatch* m_physicalDeviceDispatch;
    const vkroots::VkDeviceDispatch* m_deviceDispatch;

#ifndef _WIN32
#include "vulkan_queue_linux.inc"
#endif
};