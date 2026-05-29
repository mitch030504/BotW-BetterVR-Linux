#include "pch.h"
#include "layer_linux.h"

#include "instance.h"
#include "rendering/vulkan.h"
#include "rendering/openxr.h"

#include <cstring>
#include <algorithm>

// Linux Vulkan layer/device/session setup, moved verbatim out of layer.cpp / instance.h
// to keep those pre-existing files minimal for upstream review. No logic changes.

namespace VRLayer {

void LinuxAddRequiredInstanceExtensions(std::vector<const char*>& modifiedExtensions) {
    // SteamVR needs vkGetPhysicalDeviceProperties2KHR (and external-* capabilities) to
    // find the GPU; inject them if Cemu didn't already request them.
    const std::vector<const char*> requiredInstanceExtensions = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    };
    for (const char* ext : requiredInstanceExtensions) {
        bool alreadyEnabled = false;
        for (const char* existing : modifiedExtensions) {
            if (existing && std::strcmp(existing, ext) == 0) {
                alreadyEnabled = true;
                break;
            }
        }
        if (!alreadyEnabled) {
            modifiedExtensions.push_back(ext);
        }
    }
}

void LinuxAddRequiredDeviceExtensions(std::vector<std::string>& exts) {
    // On Linux we can't query OpenXR for required extensions during Vulkan device
    // creation (SteamVR's runtime calls Vulkan internally, deadlocking the loader).
    // Inject the standard set SteamVR/Monado require for Vulkan-based OpenXR sessions.
    const std::vector<std::string> linuxXrExtensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    };
    for (const auto& ext : linuxXrExtensions) {
        exts.push_back(ext);
    }
}

void LinuxPatchDeviceQueues(VkDeviceCreateInfo& modifiedCreateInfo,
        const std::vector<VkQueueFamilyProperties>& queueFamilies,
        std::vector<VkDeviceQueueCreateInfo>& patchedQueueInfos,
        std::vector<std::vector<float>>& patchedPriorities,
        int32_t& requestedSubmitQueueIndex, int32_t& requestedCopyQueueIndex) {
    // Find the first graphics-capable queue create info.
    int targetIdx = -1;
    for (uint32_t i = 0; i < modifiedCreateInfo.queueCreateInfoCount; i++) {
        uint32_t famIdx = modifiedCreateInfo.pQueueCreateInfos[i].queueFamilyIndex;
        if (famIdx < queueFamilies.size() &&
            (queueFamilies[famIdx].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            targetIdx = (int)i;
            break;
        }
    }
    if (targetIdx >= 0) {
        const auto& targetQci = modifiedCreateInfo.pQueueCreateInfos[targetIdx];
        uint32_t famQueueCount = queueFamilies[targetQci.queueFamilyIndex].queueCount;
        uint32_t available = famQueueCount - targetQci.queueCount;
        uint32_t extraToRequest = std::min<uint32_t>(2u, available);
        if (extraToRequest > 0) {
            // Deep-copy all queue create infos so we can bump targetIdx.
            patchedQueueInfos.assign(
                modifiedCreateInfo.pQueueCreateInfos,
                modifiedCreateInfo.pQueueCreateInfos + modifiedCreateInfo.queueCreateInfoCount);
            patchedPriorities.resize(patchedQueueInfos.size());
            for (size_t i = 0; i < patchedQueueInfos.size(); i++) {
                const auto& orig = modifiedCreateInfo.pQueueCreateInfos[i];
                patchedPriorities[i].assign(orig.pQueuePriorities,
                                            orig.pQueuePriorities + orig.queueCount);
                if ((int)i == targetIdx) {
                    for (uint32_t k = 0; k < extraToRequest; k++) {
                        patchedPriorities[i].push_back(0.5f); // slightly below main
                    }
                    patchedQueueInfos[i].queueCount = orig.queueCount + extraToRequest;
                    // Index of first extra queue (runtime); second extra is copy.
                    requestedSubmitQueueIndex = (int32_t)orig.queueCount;
                    if (extraToRequest >= 2) {
                        requestedCopyQueueIndex = (int32_t)(orig.queueCount + 1);
                    }
                }
                patchedQueueInfos[i].pQueuePriorities = patchedPriorities[i].data();
            }
            modifiedCreateInfo.pQueueCreateInfos = patchedQueueInfos.data();
            if (extraToRequest >= 2) {
                Log::print<INFO>("Requesting 2 extra VR queues: family={} runtime={} copy={} (family has {} queues)",
                    targetQci.queueFamilyIndex, requestedSubmitQueueIndex, requestedCopyQueueIndex, famQueueCount);
            } else {
                Log::print<INFO>("Requesting 1 extra VR queue: family={} runtime+copy={} (family only has {} queues, copy will share)",
                    targetQci.queueFamilyIndex, requestedSubmitQueueIndex, famQueueCount);
            }
        } else {
            Log::print<INFO>("Graphics queue family {} is fully allocated ({}/{} queues); VR work will share Cemu's queue",
                targetQci.queueFamilyIndex, targetQci.queueCount, famQueueCount);
        }
    }
}

void LinuxPublishQueueIndices(int32_t requestedSubmitQueueIndex, int32_t requestedCopyQueueIndex) {
    // Publish the requested queue indices so VRManager::Init can apply them when
    // RND_Vulkan is constructed later (VK may not exist yet on this path).
    if (requestedSubmitQueueIndex >= 0) {
        g_requestedSubmitQueueIndex.store(requestedSubmitQueueIndex);
        if (VRManager::instance().VK) {
            VRManager::instance().VK->SetSubmitQueueIndex(requestedSubmitQueueIndex);
        }
    }
    if (requestedCopyQueueIndex >= 0) {
        g_requestedCopyQueueIndex.store(requestedCopyQueueIndex);
        if (VRManager::instance().VK) {
            VRManager::instance().VK->SetCopyQueueIndex(requestedCopyQueueIndex);
        }
    }
}

void LinuxApplyVulkanQueueSetup(RND_Vulkan* vk, VkPhysicalDevice physicalDevice) {
    // Apply the dedicated queue indices from CreateDevice (if any).
    int32_t submitIdx = g_requestedSubmitQueueIndex.load();
    if (submitIdx >= 0) {
        vk->SetSubmitQueueIndex(submitIdx);
        Log::print<INFO>("Applied dedicated OpenXR runtime queue index: {}", submitIdx);
    }
    int32_t copyIdx = g_requestedCopyQueueIndex.load();
    if (copyIdx >= 0) {
        vk->SetCopyQueueIndex(copyIdx);
        Log::print<INFO>("Applied dedicated copy queue index: {}", copyIdx);
    }
    // Find the graphics queue family index for the OpenXR Vulkan binding.
    auto* physDispatch = vkroots::tables::PhysicalDeviceDispatches.find(physicalDevice);
    if (physDispatch) {
        uint32_t queueFamilyCount = 0;
        physDispatch->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        physDispatch->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                vk->SetQueueInfo(i, 0);
                Log::print<INFO>("Using queue family {} for OpenXR Vulkan binding", i);
                break;
            }
        }
    }
}

void LinuxCreateVulkanSession(OpenXR* xr, RND_Vulkan* vk) {
    // Query the physical device OpenXR wants us to use.
    VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;
    if (xr->func_xrGetVulkanGraphicsDeviceKHR) {
        XrResult xrResult = xr->func_xrGetVulkanGraphicsDeviceKHR(
            xr->GetInstance(), xr->GetSystemId(), vk->GetInstance(), &xrPhysicalDevice);
        if (XR_SUCCEEDED(xrResult) && xrPhysicalDevice != VK_NULL_HANDLE) {
            Log::print<INFO>("OpenXR selected VkPhysicalDevice: {} (ours: {}, match: {})",
                (void*)xrPhysicalDevice, (void*)vk->GetPhysicalDevice(),
                xrPhysicalDevice == vk->GetPhysicalDevice() ? "yes" : "NO");
        }
    }

    XrGraphicsBindingVulkanKHR vulkanBinding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    vulkanBinding.instance = vk->GetInstance();
    vulkanBinding.physicalDevice = xrPhysicalDevice != VK_NULL_HANDLE ? xrPhysicalDevice : vk->GetPhysicalDevice();
    vulkanBinding.device = vk->GetDevice();
    vulkanBinding.queueFamilyIndex = vk->GetQueueFamilyIndex();
    // If CreateDevice gave us a dedicated submit queue, hand it to OpenXR; the runtime's
    // compositor work runs on it (in our submit thread), parallel to Cemu's queue 0.
    // Concurrent vkQueueSubmit on queue 0 (Cemu + runtime) is UB and hangs.
    vulkanBinding.queueIndex = vk->HasDedicatedSubmitQueue()
        ? (uint32_t)vk->GetSubmitQueueIndex()
        : vk->GetQueueIndex();
    Log::print<INFO>("OpenXR Vulkan binding using queue index {} ({})",
        vulkanBinding.queueIndex,
        vk->HasDedicatedSubmitQueue() ? "DEDICATED submit queue" : "shared with Cemu");

    Log::print<INFO>("Vulkan binding: instance={}, physDev={}, device={}, queueFamily={}, queueIdx={}",
        (void*)vulkanBinding.instance, (void*)vulkanBinding.physicalDevice,
        (void*)vulkanBinding.device, vulkanBinding.queueFamilyIndex, vulkanBinding.queueIndex);

    xr->CreateSession(&vulkanBinding, "Failed to create Vulkan-based OpenXR session!");
}

}
