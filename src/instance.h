#pragma once

#include <pch.h>

#include "hooking/cemu_hooks.h"
#ifdef _WIN32
#include "rendering/d3d12.h"
#endif
#include "rendering/openxr.h"
#include "rendering/renderer.h"
#include "rendering/vulkan.h"
#ifndef _WIN32
#include "hooking/layer_linux.h"
#endif

class VRManager {
public:
    static VRManager& instance() {
        static VRManager singletonInstance;
        return singletonInstance;
    }

    VRManager(VRManager const&) = delete;
    void operator=(VRManager const&) = delete;

    void Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device) {
#ifdef _WIN32
        D3D12 = std::make_unique<RND_D3D12>();
#else
        // On Linux, create OpenXR here (deferred from constructor to avoid
        // deadlocking with the Vulkan loader during library load)
        if (!XR) {
            XR = std::make_unique<OpenXR>();
        }
#endif
        VK = std::make_unique<RND_Vulkan>(instance, physicalDevice, device);
#ifndef _WIN32
        VRLayer::LinuxApplyVulkanQueueSetup(VK.get(), physicalDevice);
#endif
        Log::print<INFO>("Initialized VRManager instance...");
    }

    void InitSession() {
#ifdef _WIN32
        XrGraphicsBindingD3D12KHR d3d12Binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        d3d12Binding.device = D3D12->GetDevice();
        d3d12Binding.queue = D3D12->GetCommandQueue();
        XR->CreateSession(&d3d12Binding, "Failed to create D3D12-based OpenXR session!");
#else
        VRLayer::LinuxCreateVulkanSession(XR.get(), VK.get());
#endif
        XR->CreateActions();
        Hooks = std::make_unique<CemuHooks>();
    }

    std::unique_ptr<OpenXR> XR;
#ifdef _WIN32
    std::unique_ptr<RND_D3D12> D3D12;
#endif
    std::unique_ptr<RND_Vulkan> VK;
    std::unique_ptr<CemuHooks> Hooks;

    uint32_t vkVersion = 0;

private:
    VRManager() {
        m_logger = std::make_unique<Log>();
#ifdef _WIN32
        // On Windows, OpenXR must be created early for LUID-based GPU filtering.
        // The Windows Vulkan loader doesn't deadlock during DLL load.
        XR = std::make_unique<OpenXR>();
#endif
        // On Linux, OpenXR is created lazily in Init() to avoid deadlocking
        // with the Vulkan loader (SteamVR's OpenXR runtime calls Vulkan internally).
    };

    ~VRManager() {
        // note: OpenXR gets to remove its swapchains first before D3D12 gets destroyed, so reverse that order
        VK.reset();
        XR.reset();
#ifdef _WIN32
        D3D12.reset();
#endif

        m_logger.reset();
    };

    std::unique_ptr<Log> m_logger;
};
