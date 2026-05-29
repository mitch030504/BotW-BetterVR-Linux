#include "pch.h"

#include "framebuffer.h"
#include "instance.h"
#include "layer.h"
#include "utils/vulkan_utils.h"
#include "utils/debug_draw.h"
#include "utils/render_utils.h"

#ifndef _WIN32
#include "rendering/linux_desktop_mirror.h"
#include "framebuffer_linux_diag.h"
#include <cstdlib>
#include <cstring>
#endif

std::mutex lockImageResolutions;
std::unordered_map<VkImage, std::pair<VkExtent2D, VkFormat>> imageResolutions;

#ifdef _WIN32
std::mutex s_activeCopyMutex;
std::vector<std::pair<VkCommandBuffer, SharedTexture*>> s_activeCopyOperations;
#else
static bool s_hudCapturedThisFrame = false;
#endif

VkImage s_curr3DColorImage = VK_NULL_HANDLE;
VkImage s_curr3DDepthImage = VK_NULL_HANDLE;

using namespace VRLayer;

VkResult VkDeviceOverrides::CreateImage(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.CreateImage(device, pCreateInfo, pAllocator, pImage);
    }

    VkResult res = pDispatch.CreateImage(device, pCreateInfo, pAllocator, pImage);

    if (pCreateInfo->extent.width >= 1280 && pCreateInfo->extent.height >= 720) {
        lockImageResolutions.lock();
        checkAssert(imageResolutions.try_emplace(*pImage, std::make_pair(VkExtent2D{ pCreateInfo->extent.width, pCreateInfo->extent.height }, pCreateInfo->format)).second, "Couldn't insert image resolution into map!");
        lockImageResolutions.unlock();
    }
    return res;
}

void VkDeviceOverrides::DestroyImage(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.DestroyImage(device, image, pAllocator);
    }

    lockImageResolutions.lock();
    imageResolutions.erase(image);
    if (s_curr3DColorImage == image) {
        s_curr3DColorImage = VK_NULL_HANDLE;
    }
    else if (s_curr3DDepthImage == image) {
        s_curr3DDepthImage = VK_NULL_HANDLE;
    }
    lockImageResolutions.unlock();

    pDispatch.DestroyImage(device, image, pAllocator);
}


void CemuHooks::hook_FixCameraSaveFilesAndInventory(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t originCaller = hCPU->gpr[0];
    uint32_t isEnabling3DFramebufferCapture = hCPU->gpr[3];
    EyeSide side = (EyeSide)hCPU->gpr[4];
    uint32_t frameIdx = hCPU->gpr[5];

    Log::print<PPC>("[{:08X}] hook_FixCameraSaveFilesAndInventory: isEnabling3DFramebufferCapture={:08X}, side={}, frameIdx={}", originCaller, isEnabling3DFramebufferCapture, side, frameIdx);
    VRManager::instance().XR->GetRenderer()->SignalGameCapturing3DFrameBuffer();
}


void VkDeviceOverrides::CmdClearColorImage(const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
    }

    // check whether the magic values are there, and which order they are in to determine which eye
    OpenXR::EyeSide side = (OpenXR::EyeSide)-1;
    if (pColor->float32[1] >= 0.12 && pColor->float32[1] <= 0.13 && pColor->float32[2] >= 0.97 && pColor->float32[2] <= 0.99) {
        side = OpenXR::EyeSide::LEFT;
    }
    else if (pColor->float32[2] >= 0.12 && pColor->float32[2] <= 0.13 && pColor->float32[1] >= 0.97 && pColor->float32[1] <= 0.99) {
        side = OpenXR::EyeSide::RIGHT;
    }

    if (!VRManager::instance().VK) {
        auto* dispatch = pDispatch.pDeviceDispatch;
        VRManager::instance().Init(dispatch->pPhysicalDeviceDispatch->pInstanceDispatch->Instance, dispatch->PhysicalDevice, dispatch->Device);
        VRManager::instance().InitSession();
    }

    if (side != (OpenXR::EyeSide)-1) {
        // r value in magical clear value is the capture idx after rounding down
        const long captureIdx = std::lroundf(pColor->float32[0] * 32.0f);
        const long frameIdx = pColor->float32[3] < 0.5f ? 0 : 1;
        checkAssert(captureIdx == 0 || captureIdx == 2, "Invalid capture index!");

        Log::print<RENDERING>("[{}] Clearing color image for {} layer for {} side", frameIdx, captureIdx == 0 ? "3D" : "2D", side == OpenXR::EyeSide::LEFT ? "left" : "right");

        auto* renderer = VRManager::instance().XR->GetRenderer();
        if (!renderer) {
            Log::print<RENDERING>("Renderer is not initialized yet!");
            return pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
        }
        auto& imguiOverlay = renderer->m_imguiOverlay;

#ifdef _WIN32
        auto& layer3D = renderer->m_layer3D;
        auto& layer2D = renderer->m_layer2D;

        // initialize the textures of both 2D and 3D layer if either is found since they share the same VkImage and resolution
        if (captureIdx == 0 || captureIdx == 2) {
            if (!layer2D) {
                lockImageResolutions.lock();
                if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                    auto viewConfs = VRManager::instance().XR->GetViewConfigurations();

                    VkExtent2D renderRes = it->second.first;
                    VkExtent2D swapchainRes = it->second.first;
                    if (VRManager::instance().XR->m_capabilities.isMetaSimulator) {
                        swapchainRes = VkExtent2D{ viewConfs[0].recommendedImageRectWidth, viewConfs[0].recommendedImageRectHeight };
                    }

                    renderer->m_gameRenderAspectRatio = (float)renderRes.width / (float)renderRes.height;
                    layer3D = std::make_unique<RND_Renderer::Layer3D>(renderRes, swapchainRes);
                    layer2D = std::make_unique<RND_Renderer::Layer2D>(renderRes, swapchainRes);
                    for (auto& textures : layer3D->GetSharedTextures()) {
                        for (auto& texture : textures) {
                            texture->Init(commandBuffer);
                        }
                    }
                    for (auto& textures : layer3D->GetDepthSharedTextures()) {
                        for (auto& texture : textures) {
                            texture->Init(commandBuffer);
                        }
                    }
                    for (auto& texture : layer2D->GetSharedTextures()) {
                        texture->Init(commandBuffer);
                    }

                    Log::print<INFO>("Found rendering resolution {}x{} @ {} using capture #{}", renderRes.width, renderRes.height, it->second.second, captureIdx);
                    imguiOverlay = std::make_unique<RND_Renderer::ImGuiOverlay>(commandBuffer, renderRes, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                    VRManager::instance().Hooks->m_entityDebugger = std::make_unique<EntityDebugger>();
                }
                else {
                    checkAssert(false, "Couldn't find image resolution in map!");
                }
                lockImageResolutions.unlock();
            }
        }

        if (!VRManager::instance().XR->GetRenderer()->IsInitialized()) {
            return;
        }

        checkAssert(layer3D && layer2D, "Couldn't find 3D or 2D layer!");

        // change source image to GENERAL layout
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL);
        VulkanUtils::DebugPipelineBarrier(commandBuffer);

        auto returnToLayout = [&]() {
            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VulkanUtils::DebugPipelineBarrier(commandBuffer);
        };

        RND_Renderer::RenderFrame& frame = renderer->GetFrame(frameIdx);

        auto clearFramebuffer = [&](bool disableAlpha) -> void {
            VkClearColorValue clearColor = disableAlpha ? VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 1.0f } } : VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, &clearColor, rangeCount, pRanges);
        };

        // 3D layer - color texture for 3D rendering
        if (captureIdx == 0) {
            // check if the color texture has the appropriate texture format
            if (s_curr3DColorImage == VK_NULL_HANDLE) {
                lockImageResolutions.lock();
                if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                    if (it->second.second == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                        s_curr3DColorImage = it->first;
                    }
                }
                lockImageResolutions.unlock();
            }

            // don't clear the image if we're in the faux 2D mode
            if (CemuHooks::UseBlackBarsDuringEvents()) {
                returnToLayout();
                return;
            }

            if (image != s_curr3DColorImage) {
                Log::print<RENDERING>("Color image is not the same as the current 3D color image! ({} != {})", (void*)image, (void*)s_curr3DColorImage);
                returnToLayout();
                return clearFramebuffer(!VRManager::instance().XR->GetRenderer()->IsRendering3D(frameIdx));
            }

            if (renderer->GetFrame(frameIdx).copiedColor[side]) {
                // the color texture has already been copied to the layer
                Log::print<RENDERING>("A 3D color texture is already been copied for the current frame!");

                returnToLayout();
                if (CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
                    return;
                }
                return clearFramebuffer(false);
            }

            // note: This uses vkCmdCopyImage to copy the image to the D3D12-created interop texture. s_activeCopyOperations queues a semaphore for the D3D12 side to wait on.
            SharedTexture* texture = layer3D->CopyColorToLayer(side, commandBuffer, image, frameIdx);
            renderer->On3DColorCopied(side, frameIdx);

            {
                std::lock_guard lk(s_activeCopyMutex);
                s_activeCopyOperations.emplace_back(commandBuffer, texture);
            }

            if (CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
                return;
            }

            // imgui needs only one eye to render Cemu's 2D output, so use right side since it looks better
            if (side == EyeSide::RIGHT) {
                // note: Uses vkCmdCopyImage to copy the (right-eye-only) image to the imgui overlay's texture
                float desktopAspectRatio = layer3D->GetAspectRatio(side);
                const RenderUtils::UvTransform& desktopUvTransform = layer3D->GetPresentUvTransform(side);
                imguiOverlay->Draw3DLayerAsBackground(commandBuffer, image, desktopAspectRatio, desktopUvTransform, frameIdx);
            }

            // clear the image to be transparent to allow for the HUD to be rendered on top of it which results in a transparent HUD layer
            returnToLayout();
            return clearFramebuffer(false);
        }

        // 2D layer - color texture for HUD rendering
        if (captureIdx == 2) {
            bool hudCopied = renderer->GetFrame(frameIdx).copied2D;

            if (side == EyeSide::LEFT) {
                if (hudCopied) {
                    // the 2D texture has already been copied to the layer
                    Log::print<RENDERING>("A 2D texture has already been copied for the current frame!");

                    returnToLayout();
                    return clearFramebuffer(false);
                }
                else {
                    // provide the HUD texture to the imgui overlay we'll use to recomposite Cemu's original flatscreen rendering
                    if (imguiOverlay && !hudCopied) {
                        imguiOverlay->DrawHUDLayerAsBackground(commandBuffer, image, frameIdx);
                        VulkanUtils::DebugPipelineBarrier(commandBuffer);
                    }

                    if (imguiOverlay && !hudCopied) {
                        // render imgui, and then copy the framebuffer to the 2D layer
                        imguiOverlay->Update();
                        imguiOverlay->Render(frameIdx, false, false);
                        imguiOverlay->DrawAndCopyToImage(commandBuffer, image, frameIdx, false);
                        VulkanUtils::DebugPipelineBarrier(commandBuffer);
                    }

                    // copy the HUD texture to D3D12 to be presented
                    // only copy the first attempt at capturing when GX2ClearColor is called with this capture index since the game/Cemu clears the 2D layer twice
                    SharedTexture* texture = layer2D->CopyColorToLayer(commandBuffer, image, frameIdx);
                    renderer->On2DCopied(frameIdx);

                    returnToLayout();
                    {
                        std::lock_guard lk(s_activeCopyMutex);
                        s_activeCopyOperations.emplace_back(commandBuffer, texture);
                    }
                    return;
                }
            }
            if (side == EyeSide::RIGHT) {
                // render the imgui overlay on the right side
                if (imguiOverlay) {
                    // render imgui, and then copy the framebuffer to the 2D layer
                    imguiOverlay->Render(frameIdx, true, true);
                    imguiOverlay->Update();
                    imguiOverlay->DrawAndCopyToImage(commandBuffer, image, frameIdx, true);

                    returnToLayout();
                    return;
                }

                if (hudCopied) {
                    returnToLayout();
                    return clearFramebuffer(false);
                }
            }
        }
        returnToLayout();
        return;
#else // !_WIN32
        // Linux capture lives in renderer_vulkan.cpp; the hook only looks up the source
        // image's tracked dimensions and dispatches.
        uint32_t srcWidth = 0, srcHeight = 0;
        VkFormat srcFormat = VK_FORMAT_UNDEFINED;
        {
            std::lock_guard lk(lockImageResolutions);
            if (auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                srcWidth = it->second.first.width;
                srcHeight = it->second.first.height;
                srcFormat = it->second.second;
            }
        }
        renderer->LinuxHandleColorClear(side, captureIdx, frameIdx, pDispatch, commandBuffer, image,
            imageLayout, pColor, rangeCount, pRanges, srcWidth, srcHeight, srcFormat, s_hudCapturedThisFrame);
        return;
#endif // _WIN32
    }
    else {
        return pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
    }
}

void VkDeviceOverrides::CmdClearDepthStencilImage(const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }

    // check for magical clear values
    // check order and whether there's a match with the magical clear value
    OpenXR::EyeSide side = (OpenXR::EyeSide)-1;
    if (pDepthStencil->depth >= 0.011456789 && pDepthStencil->depth <= 0.013456789) { // 0.0123456789
        side = OpenXR::EyeSide::LEFT;
    }
    else if (pDepthStencil->depth >= 0.153987654 && pDepthStencil->depth <= 0.173987654) { // 0.163987654
        side = OpenXR::EyeSide::RIGHT;
    }

    if (rangeCount == 1 && side != (OpenXR::EyeSide)-1) {
#ifdef _WIN32
        // stencil value is the frame counter
        const uint32_t frameCounter = pDepthStencil->stencil;
        checkAssert(frameCounter == 0 || frameCounter == 1, "Invalid frame counter for depth clear!");

        auto& layer3D = VRManager::instance().XR->GetRenderer()->m_layer3D;
        auto& layer2D = VRManager::instance().XR->GetRenderer()->m_layer2D;

        if (!VRManager::instance().XR->GetRenderer()->IsInitialized()) {
            return;
        }

        Log::print<RENDERING>("[{}] Clearing depth image for 3D layer for {} side", frameCounter, side == OpenXR::EyeSide::LEFT ? "left" : "right");

        // change source image to GENERAL layout
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL);
        VulkanUtils::DebugPipelineBarrier(commandBuffer);

        auto returnToLayout = [&]() {
            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VulkanUtils::DebugPipelineBarrier(commandBuffer);
        };

        if (side == OpenXR::EyeSide::LEFT || side == OpenXR::EyeSide::RIGHT) {
            // 3D layer - depth texture for 3D rendering
            if (s_curr3DDepthImage == VK_NULL_HANDLE) {
                lockImageResolutions.lock();
                if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                    if (it->second.second == VK_FORMAT_D32_SFLOAT) {
                        s_curr3DDepthImage = it->first;
                    }
                }
                lockImageResolutions.unlock();
            }

            if (image != s_curr3DDepthImage) {
                Log::print<RENDERING>("Depth image is not the same as the current 3D depth image! ({} != {})", (void*)image, (void*)s_curr3DDepthImage);
                returnToLayout();
                return;
            }

            if (VRManager::instance().XR->GetRenderer()->GetFrame(frameCounter).copiedDepth[side]) {
                // the depth texture has already been copied to the layer
                Log::print<RENDERING>("A depth texture is already bound for the current frame!");
                returnToLayout();
                return;
            }

            SharedTexture* texture = layer3D->CopyDepthToLayer(side, commandBuffer, image, frameCounter);
            VRManager::instance().XR->GetRenderer()->On3DDepthCopied(side, frameCounter);

            {
                std::lock_guard lk(s_activeCopyMutex);
                s_activeCopyOperations.emplace_back(commandBuffer, texture);
            }
            returnToLayout();
            return;
        }
#else // !_WIN32
        // Linux capture lives in renderer_vulkan.cpp; the hook only looks up the source
        // image's tracked dimensions and dispatches.
        auto* renderer = VRManager::instance().XR->GetRenderer();
        if (!renderer || !renderer->IsInitialized()) {
            return pDispatch.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
        }
        uint32_t srcWidth = 0, srcHeight = 0;
        {
            std::lock_guard lk(lockImageResolutions);
            if (auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                srcWidth = it->second.first.width;
                srcHeight = it->second.first.height;
            }
        }
        renderer->LinuxHandleDepthClear(side, pDispatch, commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges, srcWidth, srcHeight);
        return;
#endif // _WIN32
    }
    else {
        return pDispatch.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }
}

VkResult VkDeviceOverrides::QueueSubmit(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.QueueSubmit(queue, submitCount, pSubmits, fence);
    }

    VkResult result = VK_SUCCESS;

#ifdef _WIN32
    size_t activeCopyCount;
    {
        std::lock_guard lk(s_activeCopyMutex);
        activeCopyCount = s_activeCopyOperations.size();
    }

    if (activeCopyCount == 0) {
        result = pDispatch.QueueSubmit(queue, submitCount, pSubmits, fence);
    }
    else {
        struct ModifiedSubmitInfo_t {
            VkSubmitInfo submitInfoCopy; // Shadow copy of VkSubmitInfo
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<uint64_t> timelineWaitValues;
            std::vector<VkPipelineStageFlags> waitDstStageMasks;
            std::vector<VkSemaphore> signalSemaphores;
            std::vector<uint64_t> timelineSignalValues;

            VkTimelineSemaphoreSubmitInfo timelineSemaphoreSubmitInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        };

        // insert (possible) pipeline barriers for any active copy operations
        std::vector<ModifiedSubmitInfo_t> modifiedSubmitInfos{ submitCount };
        std::vector<VkSubmitInfo> shadowSubmits{ submitCount };

        std::lock_guard lk(s_activeCopyMutex);

        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo& submitInfo = pSubmits[i];
            ModifiedSubmitInfo_t& modifiedSubmitInfo = modifiedSubmitInfos[i];

            // AMD GPU FIX: Create shadow copy of original VkSubmitInfo
            modifiedSubmitInfo.submitInfoCopy = submitInfo;

            // copy old semaphores into new vectors
            modifiedSubmitInfo.waitSemaphores.assign(submitInfo.pWaitSemaphores, submitInfo.pWaitSemaphores + submitInfo.waitSemaphoreCount);
            modifiedSubmitInfo.waitDstStageMasks.assign(submitInfo.pWaitDstStageMask, submitInfo.pWaitDstStageMask + submitInfo.waitSemaphoreCount);
            modifiedSubmitInfo.timelineWaitValues.resize(submitInfo.waitSemaphoreCount, 0);

            modifiedSubmitInfo.signalSemaphores.assign(submitInfo.pSignalSemaphores, submitInfo.pSignalSemaphores + submitInfo.signalSemaphoreCount);
            modifiedSubmitInfo.timelineSignalValues.resize(submitInfo.signalSemaphoreCount, 0);

            // find timeline semaphore submit info if already present
            const VkTimelineSemaphoreSubmitInfo* existingTimelineInfo = nullptr;

            const VkBaseInStructure* pNextIt = static_cast<const VkBaseInStructure*>(submitInfo.pNext);
            while (pNextIt) {
                if (pNextIt->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    existingTimelineInfo = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(pNextIt);
                    break;
                }
                pNextIt = pNextIt->pNext;
            }

            // copy any existing timeline values into new vectors
            if (existingTimelineInfo) {
                for (uint32_t j = 0; j < existingTimelineInfo->waitSemaphoreValueCount; j++) {
                    modifiedSubmitInfo.timelineWaitValues[j] = existingTimelineInfo->pWaitSemaphoreValues[j];
                }
                for (uint32_t j = 0; j < existingTimelineInfo->signalSemaphoreValueCount; j++) {
                    modifiedSubmitInfo.timelineSignalValues[j] = existingTimelineInfo->pSignalSemaphoreValues[j];
                }
            }

            // Insert timeline semaphores for active copy operations
            for (uint32_t j = 0; j < submitInfo.commandBufferCount; j++) {
                for (auto it = s_activeCopyOperations.begin(); it != s_activeCopyOperations.end();) {
                    if (submitInfo.pCommandBuffers[j] == it->first) {
                        // Wait for D3D12/XR to finish with the previous shared texture render
                        uint64_t waitValue = it->second->GetVulkanWaitValue();
                        modifiedSubmitInfo.waitSemaphores.emplace_back(it->second->GetSemaphoreForWait(waitValue));
                        modifiedSubmitInfo.waitDstStageMasks.emplace_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                        modifiedSubmitInfo.timelineWaitValues.emplace_back(waitValue);

                        // Signal to D3D12/XR rendering that the shared texture can be rendered to VR headset
                        uint64_t signalValue = it->second->GetVulkanSignalValue();
                        modifiedSubmitInfo.signalSemaphores.emplace_back(it->second->GetSemaphoreForSignal(signalValue));
                        modifiedSubmitInfo.timelineSignalValues.emplace_back(signalValue);
                        it = s_activeCopyOperations.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }

            // Update timeline semaphore submit info
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.waitSemaphoreValueCount = (uint32_t)modifiedSubmitInfo.timelineWaitValues.size();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pWaitSemaphoreValues = modifiedSubmitInfo.timelineWaitValues.data();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.signalSemaphoreValueCount = (uint32_t)modifiedSubmitInfo.timelineSignalValues.size();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pSignalSemaphoreValues = modifiedSubmitInfo.timelineSignalValues.data();

            // AMD GPU FIX: Preserve existing pNext chain - prepend our timeline struct
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pNext = submitInfo.pNext;

            modifiedSubmitInfo.submitInfoCopy.pNext = &modifiedSubmitInfo.timelineSemaphoreSubmitInfo;
            modifiedSubmitInfo.submitInfoCopy.waitSemaphoreCount = (uint32_t)modifiedSubmitInfo.waitSemaphores.size();
            modifiedSubmitInfo.submitInfoCopy.pWaitSemaphores = modifiedSubmitInfo.waitSemaphores.data();
            modifiedSubmitInfo.submitInfoCopy.pWaitDstStageMask = modifiedSubmitInfo.waitDstStageMasks.data();
            modifiedSubmitInfo.submitInfoCopy.signalSemaphoreCount = (uint32_t)modifiedSubmitInfo.signalSemaphores.size();
            modifiedSubmitInfo.submitInfoCopy.pSignalSemaphores = modifiedSubmitInfo.signalSemaphores.data();

            shadowSubmits[i] = modifiedSubmitInfo.submitInfoCopy;
        }
        result = pDispatch.QueueSubmit(queue, submitCount, shadowSubmits.data(), fence);
    }
#else // !_WIN32
    result = pDispatch.QueueSubmit(queue, submitCount, pSubmits, fence);
#endif // _WIN32

    if (result != VK_SUCCESS) {
        // Rate-limit so a sustained DEVICE_LOST doesn't flood the log with
        // thousands of identical messages.
        static std::atomic<int> s_errCount{0};
        static std::atomic<VkResult> s_lastErr{VK_SUCCESS};
        if (result != s_lastErr.load() || s_errCount.fetch_add(1) < 5) {
            Log::print<ERROR>("QueueSubmit failed with error {}", result);
            s_lastErr.store(result);
        }
    }

    return result;
}

VkResult VkDeviceOverrides::QueuePresentKHR(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!VRLayer::IsLayerActiveForProcess()) {
        return pDispatch.QueuePresentKHR(queue, pPresentInfo);
    }

#ifndef _WIN32
    // Reset HUD capture flag each frame so menus without 3D rendering still get captured
    s_hudCapturedThisFrame = false;
#endif
    VRManager::instance().XR->ProcessEvents();

    auto* renderer = VRManager::instance().XR->GetRenderer();
#ifdef _WIN32
    if (renderer && renderer->m_layer3D && renderer->m_layer2D && renderer->m_imguiOverlay) {
#else
    if (renderer && renderer->IsInitialized()) {
#endif
        if (renderer->IsInitialized()) {
            renderer->EndFrame();
        }
        renderer->StartFrame();
    }

#ifdef _WIN32
    return pDispatch.QueuePresentKHR(queue, pPresentInfo);
#else
    return LinuxDesktopMirror::QueuePresentKHR(pDispatch, queue, pPresentInfo);
#endif
}
