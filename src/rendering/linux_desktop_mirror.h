#pragma once

#ifndef _WIN32

namespace LinuxDesktopMirror {
    bool IsEnabled();

    // Capture hooks pass images in VK_IMAGE_LAYOUT_GENERAL while Cemu is
    // recording its own command buffer. The mirror helper copies into
    // BetterVR-owned source textures and leaves them in TRANSFER_SRC.
    void CaptureColor(int sideIdx, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx);
    void CaptureHud(VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, int bufferIdx);

    VkResult CreateSwapchainKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
    VkResult GetSwapchainImagesKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
    void DestroySwapchainKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
    VkResult QueuePresentKHR(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    void Shutdown();
}

#endif
