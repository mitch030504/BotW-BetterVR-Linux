#pragma once
// Linux-only Vulkan layer/device/session setup, extracted verbatim from the shared
// layer.cpp and instance.h so those pre-existing files stay close to upstream.
// Compiled only on Linux (see src/CMakeLists.txt). Declared in namespace VRLayer.
//
// Relies on Vulkan/OpenXR types from pch.h (force-included into every TU).

#include <vector>
#include <string>

class RND_Vulkan;
class OpenXR;

namespace VRLayer {

// vkCreateInstance: inject the instance extensions OpenXR/SteamVR need on Linux.
void LinuxAddRequiredInstanceExtensions(std::vector<const char*>& extensions);

// Device-extension list: append the Vulkan extensions SteamVR/Monado require for a
// Vulkan-based OpenXR session (queried statically to avoid a Vulkan-loader deadlock).
void LinuxAddRequiredDeviceExtensions(std::vector<std::string>& extensions);

// vkCreateDevice: request the dedicated VR submit/copy queues (three-queue topology).
// patchedQueueInfos/patchedPriorities are caller-owned backing storage that must
// outlive the vkCreateDevice call; createInfo.pQueueCreateInfos is repointed at them.
void LinuxPatchDeviceQueues(VkDeviceCreateInfo& createInfo,
    const std::vector<VkQueueFamilyProperties>& queueFamilies,
    std::vector<VkDeviceQueueCreateInfo>& patchedQueueInfos,
    std::vector<std::vector<float>>& patchedPriorities,
    int32_t& requestedSubmitQueueIndex, int32_t& requestedCopyQueueIndex);

// vkCreateDevice (post-create): publish the requested queue indices for VRManager::Init.
void LinuxPublishQueueIndices(int32_t requestedSubmitQueueIndex, int32_t requestedCopyQueueIndex);

// VRManager::Init: apply the requested queue indices and find the graphics queue family.
void LinuxApplyVulkanQueueSetup(RND_Vulkan* vk, VkPhysicalDevice physicalDevice);

// VRManager::InitSession: build the Vulkan graphics binding and create the OpenXR session.
void LinuxCreateVulkanSession(OpenXR* xr, RND_Vulkan* vk);

}
