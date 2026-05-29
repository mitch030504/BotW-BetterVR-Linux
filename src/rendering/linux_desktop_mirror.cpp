#include "pch.h"
#ifndef _WIN32

#include "linux_desktop_mirror.h"
#include "instance.h"
#include "texture.h"
#include "utils/vulkan_utils.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

bool EnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return false;
    return std::strcmp(value, "0") != 0
        && std::strcmp(value, "false") != 0
        && std::strcmp(value, "FALSE") != 0
        && std::strcmp(value, "no") != 0
        && std::strcmp(value, "NO") != 0;
}

int ConfiguredMirrorEye() {
    const char* eye = std::getenv("BETTERVR_DESKTOP_MIRROR_EYE");
    if (eye != nullptr && (std::strcmp(eye, "left") == 0 || std::strcmp(eye, "LEFT") == 0)) {
        return 0;
    }
    return 1; // Right eye matches the Windows desktop path's recording preference.
}

struct MirrorTextureSlot {
    std::unique_ptr<VulkanTexture> texture;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;

    void Ensure(VkCommandBuffer cmdBuffer, uint32_t w, uint32_t h) {
        if (texture && width == w && height == h) {
            return;
        }

        texture = std::make_unique<VulkanTexture>(
            w,
            h,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        width = w;
        height = h;
        valid = false;
        texture->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Log::print<INFO>("Desktop mirror source texture {}x{} created", w, h);
    }
};

struct MirrorSourceState {
    std::array<MirrorTextureSlot, 2> color;
    std::array<MirrorTextureSlot, 2> hud;
    int latestColorIdx = -1;
    int latestHudIdx = -1;
    uint64_t colorSeq = 0;
    uint64_t hudSeq = 0;
};

struct PerImageResources {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct SwapchainState {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    VkImageUsageFlags usage = 0;
    bool hasTransferDst = false;
    bool resourcesReady = false;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<PerImageResources> imageResources;
};

std::mutex g_mutex;
MirrorSourceState g_sources;
std::unordered_map<VkSwapchainKHR, SwapchainState> g_swapchains;
bool g_loggedNoTransferDst = false;
bool g_loggedUnsupportedPresent = false;
bool g_loggedNoSource = false;

bool HasDevice() {
    return VRManager::instance().VK
        && VRManager::instance().VK->GetDevice() != VK_NULL_HANDLE
        && VRManager::instance().VK->GetDeviceDispatch() != nullptr;
}

void DestroySwapchainResourcesLocked(SwapchainState& state) {
    if (!HasDevice()) {
        state.imageResources.clear();
        state.commandPool = VK_NULL_HANDLE;
        state.resourcesReady = false;
        return;
    }

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkDevice device = VRManager::instance().VK->GetDevice();

    for (auto& resources : state.imageResources) {
        if (resources.fence != VK_NULL_HANDLE) {
            dispatch->DestroyFence(device, resources.fence, nullptr);
            resources.fence = VK_NULL_HANDLE;
        }
        if (resources.semaphore != VK_NULL_HANDLE) {
            dispatch->DestroySemaphore(device, resources.semaphore, nullptr);
            resources.semaphore = VK_NULL_HANDLE;
        }
        resources.commandBuffer = VK_NULL_HANDLE;
    }

    if (state.commandPool != VK_NULL_HANDLE) {
        dispatch->DestroyCommandPool(device, state.commandPool, nullptr);
        state.commandPool = VK_NULL_HANDLE;
    }

    state.imageResources.clear();
    state.resourcesReady = false;
}

bool EnsureSwapchainResourcesLocked(SwapchainState& state) {
    if (state.resourcesReady) {
        return true;
    }
    if (!HasDevice() || state.images.empty()) {
        return false;
    }

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkDevice device = VRManager::instance().VK->GetDevice();

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = VRManager::instance().VK->GetQueueFamilyIndex();
    VkResult poolResult = dispatch->CreateCommandPool(device, &poolInfo, nullptr, &state.commandPool);
    if (poolResult != VK_SUCCESS) {
        Log::print<ERROR>("Desktop mirror failed to create command pool: {}", (int)poolResult);
        return false;
    }

    state.imageResources.resize(state.images.size());
    std::vector<VkCommandBuffer> commandBuffers(state.images.size(), VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = state.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();
    VkResult allocResult = dispatch->AllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
    if (allocResult != VK_SUCCESS) {
        Log::print<ERROR>("Desktop mirror failed to allocate command buffers: {}", (int)allocResult);
        DestroySwapchainResourcesLocked(state);
        return false;
    }

    for (size_t i = 0; i < state.imageResources.size(); ++i) {
        auto& resources = state.imageResources[i];
        resources.commandBuffer = commandBuffers[i];

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkResult fenceResult = dispatch->CreateFence(device, &fenceInfo, nullptr, &resources.fence);
        if (fenceResult != VK_SUCCESS) {
            Log::print<ERROR>("Desktop mirror failed to create fence: {}", (int)fenceResult);
            DestroySwapchainResourcesLocked(state);
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkResult semResult = dispatch->CreateSemaphore(device, &semaphoreInfo, nullptr, &resources.semaphore);
        if (semResult != VK_SUCCESS) {
            Log::print<ERROR>("Desktop mirror failed to create semaphore: {}", (int)semResult);
            DestroySwapchainResourcesLocked(state);
            return false;
        }
    }

    state.resourcesReady = true;
    Log::print<INFO>("Desktop mirror resources ready for swapchain {} images={} extent={}x{}",
        (void*)state.swapchain,
        state.images.size(),
        state.extent.width,
        state.extent.height);
    return true;
}

VkRect2D FitRect(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) {
    VkRect2D rect = {};
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
        return rect;
    }

    const double srcAspect = (double)srcW / (double)srcH;
    const double dstAspect = (double)dstW / (double)dstH;
    uint32_t outW = dstW;
    uint32_t outH = dstH;
    if (dstAspect > srcAspect) {
        outW = (uint32_t)std::max(1.0, std::round((double)dstH * srcAspect));
    } else {
        outH = (uint32_t)std::max(1.0, std::round((double)dstW / srcAspect));
    }

    rect.offset.x = (int32_t)((dstW - outW) / 2);
    rect.offset.y = (int32_t)((dstH - outH) / 2);
    rect.extent = { outW, outH };
    return rect;
}

MirrorTextureSlot* SelectSourceLocked() {
    if (g_sources.latestColorIdx >= 0 && g_sources.latestColorIdx < 2) {
        auto& color = g_sources.color[g_sources.latestColorIdx];
        if (color.texture && color.valid) {
            return &color;
        }
    }
    if (g_sources.latestHudIdx >= 0 && g_sources.latestHudIdx < 2) {
        auto& hud = g_sources.hud[g_sources.latestHudIdx];
        if (hud.texture && hud.valid) {
            return &hud;
        }
    }
    return nullptr;
}

bool RecordMirrorCommandsLocked(SwapchainState& state, PerImageResources& resources, uint32_t imageIdx, MirrorTextureSlot& source) {
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkCommandBuffer cmd = resources.commandBuffer;

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult beginResult = dispatch->BeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        Log::print<ERROR>("Desktop mirror BeginCommandBuffer failed: {}", (int)beginResult);
        return false;
    }

    VkImage dstImage = state.images[imageIdx];
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dstImage;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    dispatch->CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    dispatch->CmdClearColorImage(cmd, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    VkRect2D dstRect = FitRect(source.width, source.height, state.extent.width, state.extent.height);
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (int32_t)source.width, (int32_t)source.height, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { dstRect.offset.x, dstRect.offset.y, 0 };
    blit.dstOffsets[1] = {
        dstRect.offset.x + (int32_t)dstRect.extent.width,
        dstRect.offset.y + (int32_t)dstRect.extent.height,
        1
    };
    dispatch->CmdBlitImage(cmd,
        source.texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dispatch->CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkResult endResult = dispatch->EndCommandBuffer(cmd);
    if (endResult != VK_SUCCESS) {
        Log::print<ERROR>("Desktop mirror EndCommandBuffer failed: {}", (int)endResult);
        return false;
    }
    return true;
}

} // namespace

namespace LinuxDesktopMirror {

bool IsEnabled() {
    static const bool enabled = EnvFlagEnabled("BETTERVR_DESKTOP_MIRROR");
    static std::once_flag loggedEnabled;
    if (enabled) {
        std::call_once(loggedEnabled, [] {
            Log::print<INFO>("Desktop mirror enabled: eye={}", ConfiguredMirrorEye() == 0 ? "left" : "right");
        });
    }
    return enabled;
}

void CaptureColor(int sideIdx, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx) {
    if (!IsEnabled() || sideIdx != ConfiguredMirrorEye() || frameIdx < 0 || frameIdx >= 2 || !HasDevice()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_sources.color[(int)frameIdx];
    slot.Ensure(cmdBuffer, srcWidth, srcHeight);
    if (slot.texture == nullptr) {
        return;
    }

    slot.texture->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    dispatch->CmdBlitImage(cmdBuffer,
        srcImage, VK_IMAGE_LAYOUT_GENERAL,
        slot.texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);
    slot.texture->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    slot.valid = true;
    g_sources.latestColorIdx = (int)frameIdx;
    g_sources.colorSeq++;
}

void CaptureHud(VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, int bufferIdx) {
    if (!IsEnabled() || bufferIdx < 0 || bufferIdx >= 2 || !HasDevice()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_sources.hud[bufferIdx];
    slot.Ensure(cmdBuffer, srcWidth, srcHeight);
    if (slot.texture == nullptr) {
        return;
    }

    slot.texture->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    dispatch->CmdBlitImage(cmdBuffer,
        srcImage, VK_IMAGE_LAYOUT_GENERAL,
        slot.texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);
    slot.texture->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    slot.valid = true;
    g_sources.latestHudIdx = bufferIdx;
    g_sources.hudSeq++;
}

VkResult CreateSwapchainKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    if (!IsEnabled() || pCreateInfo == nullptr) {
        return pDispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    VkSwapchainCreateInfoKHR modifiedInfo = *pCreateInfo;
    bool transferDstSupported = (pCreateInfo->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    VkSurfaceCapabilitiesKHR caps = {};
    if (!transferDstSupported &&
        pDispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(pDispatch.PhysicalDevice, pCreateInfo->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
        modifiedInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        transferDstSupported = true;
    }

    if (!transferDstSupported && !g_loggedNoTransferDst) {
        Log::print<WARNING>("Desktop mirror: Cemu swapchain does not support TRANSFER_DST; mirror disabled for this swapchain");
        g_loggedNoTransferDst = true;
    }

    VkResult result = pDispatch.CreateSwapchainKHR(device, &modifiedInfo, pAllocator, pSwapchain);
    if (result == VK_SUCCESS && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& state = g_swapchains[*pSwapchain];
        state.device = device;
        state.swapchain = *pSwapchain;
        state.format = modifiedInfo.imageFormat;
        state.extent = modifiedInfo.imageExtent;
        state.usage = modifiedInfo.imageUsage;
        state.hasTransferDst = (modifiedInfo.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
        Log::print<INFO>("Desktop mirror tracked swapchain {} extent={}x{} format={} transferDst={}",
            (void*)*pSwapchain,
            state.extent.width,
            state.extent.height,
            (int)state.format,
            state.hasTransferDst ? "yes" : "no");
    }
    return result;
}

VkResult GetSwapchainImagesKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
    VkResult result = pDispatch.GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    if (!IsEnabled() || result != VK_SUCCESS || pSwapchainImageCount == nullptr || pSwapchainImages == nullptr) {
        return result;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto it = g_swapchains.find(swapchain); it != g_swapchains.end()) {
        auto& state = it->second;
        DestroySwapchainResourcesLocked(state);
        state.images.assign(pSwapchainImages, pSwapchainImages + *pSwapchainImageCount);
        Log::print<INFO>("Desktop mirror swapchain {} images={}", (void*)swapchain, state.images.size());
    }
    return result;
}

void DestroySwapchainKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    if (IsEnabled()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_swapchains.find(swapchain); it != g_swapchains.end()) {
            DestroySwapchainResourcesLocked(it->second);
            g_swapchains.erase(it);
        }
    }
    pDispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VkResult QueuePresentKHR(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!IsEnabled() || pPresentInfo == nullptr || pPresentInfo->swapchainCount != 1 || !HasDevice()) {
        if (IsEnabled() && pPresentInfo != nullptr && pPresentInfo->swapchainCount != 1 && !g_loggedUnsupportedPresent) {
            g_loggedUnsupportedPresent = true;
            Log::print<WARNING>("Desktop mirror currently supports one swapchain per present; passing through");
        }
        return pDispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t imageIdx = pPresentInfo->pImageIndices[0];
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR mirrorPresentInfo = *pPresentInfo;
    VkSemaphore mirrorWaitSemaphore = VK_NULL_HANDLE;
    std::vector<VkPipelineStageFlags> waitStages;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto swapchainIt = g_swapchains.find(swapchain);
        if (swapchainIt == g_swapchains.end() || !swapchainIt->second.hasTransferDst || imageIdx >= swapchainIt->second.images.size()) {
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }

        SwapchainState& state = swapchainIt->second;
        if (!EnsureSwapchainResourcesLocked(state)) {
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }

        MirrorTextureSlot* source = SelectSourceLocked();
        if (source == nullptr) {
            if (!g_loggedNoSource) {
                Log::print<INFO>("Desktop mirror waiting for first captured source image");
                g_loggedNoSource = true;
            }
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }

        auto& resources = state.imageResources[imageIdx];
        auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
        VkDevice device = VRManager::instance().VK->GetDevice();
        VkResult fenceStatus = dispatch->GetFenceStatus(device, resources.fence);
        if (fenceStatus == VK_NOT_READY) {
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }
        if (fenceStatus != VK_SUCCESS) {
            Log::print<WARNING>("Desktop mirror fence status failed: {}", (int)fenceStatus);
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }

        dispatch->ResetCommandBuffer(resources.commandBuffer, 0);
        if (!RecordMirrorCommandsLocked(state, resources, imageIdx, *source)) {
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }
        dispatch->ResetFences(device, 1, &resources.fence);

        waitStages.resize(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &resources.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &resources.semaphore;

        VkResult submitResult = pDispatch.QueueSubmit(queue, 1, &submitInfo, resources.fence);
        if (submitResult != VK_SUCCESS) {
            Log::print<WARNING>("Desktop mirror QueueSubmit failed: {}", (int)submitResult);
            DestroySwapchainResourcesLocked(state);
            return pDispatch.QueuePresentKHR(queue, pPresentInfo);
        }

        mirrorWaitSemaphore = resources.semaphore;
    }

    mirrorPresentInfo.waitSemaphoreCount = 1;
    mirrorPresentInfo.pWaitSemaphores = &mirrorWaitSemaphore;
    return pDispatch.QueuePresentKHR(queue, &mirrorPresentInfo);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [_, state] : g_swapchains) {
        DestroySwapchainResourcesLocked(state);
    }
    g_swapchains.clear();
    for (auto& slot : g_sources.color) {
        slot.texture.reset();
        slot.width = 0;
        slot.height = 0;
        slot.valid = false;
    }
    for (auto& slot : g_sources.hud) {
        slot.texture.reset();
        slot.width = 0;
        slot.height = 0;
        slot.valid = false;
    }
    g_sources.latestColorIdx = -1;
    g_sources.latestHudIdx = -1;
    g_sources.colorSeq = 0;
    g_sources.hudSeq = 0;
}

} // namespace LinuxDesktopMirror

#endif
