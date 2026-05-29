#pragma once

#include <shared_mutex>
#include "pch.h"
#ifdef _WIN32
#include "d3d12.h"
#endif
#include "openxr.h"
#include "swapchain.h"
#include "texture.h"
#include "utils/render_utils.h"
#include "utils/debug_draw.h"

class SharedTexture;

class RND_Renderer {
public:
    explicit RND_Renderer(XrSession xrSession);
    ~RND_Renderer();

    struct RenderFrame {
        std::optional<std::array<XrView, 2>> views;
        std::atomic_bool copiedColor[2] = { false, false };
        std::atomic_bool copiedDepth[2] = { false, false };
        std::atomic_bool copied2D = false;
        std::atomic_bool presented3D = false;
        std::atomic_uint8_t cameraIsCapturing3DFramebuffer = 0;

        std::unique_ptr<VulkanTexture> mainFramebuffer;
        std::unique_ptr<VulkanTexture> hudFramebuffer;
        std::unique_ptr<VulkanTexture> hudWithoutAlphaFramebuffer;
        std::unique_ptr<VulkanFramebuffer> imguiFramebuffer;
        VkDescriptorSet mainFramebufferDS = VK_NULL_HANDLE;
        VkDescriptorSet hudFramebufferDS = VK_NULL_HANDLE;
        VkDescriptorSet hudWithoutAlphaFramebufferDS = VK_NULL_HANDLE;
        float mainFramebufferAspectRatio = 1.0f;
        RenderUtils::UvTransform mainFramebufferUvTransform = {};

        bool ranMotionAnalysis[2] = { false, false };

        bool Is3DComplete() const { return copiedColor[0] && copiedColor[1] && copiedDepth[0] && copiedDepth[1]; }
        bool Is2DComplete() const { return copied2D; }

        void Reset() {
            views = std::nullopt;
            copiedColor[0] = false;
            copiedColor[1] = false;
            copiedDepth[0] = false;
            copiedDepth[1] = false;
            copied2D = false;
            if (cameraIsCapturing3DFramebuffer > 0)
                --cameraIsCapturing3DFramebuffer;

            ranMotionAnalysis[0] = false;
            ranMotionAnalysis[1] = false;
        }
    };

    void StartFrame();
    void EndFrame();
    std::optional<std::array<XrView, 2>> UpdateViews(XrTime predictedDisplayTime);

    // Backend-specific members: the per-frame capture entry points, the CurrentViews
    // accessor, and (Windows) the Layer3D/Layer2D composition layers. Pure platform
    // split — the bodies are identical to the previously-inline #ifdef members.
#ifdef _WIN32
#include "renderer_members_d3d12.inc"
#else
#include "renderer_members_vulkan.inc"
#endif

    std::optional<std::array<XrView, 2>> GetPoses(long frameIdx = -1) const {
        if (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) return m_renderFrames[frameIdx].views;
        return CurrentViews();
    }

    std::optional<XrFovf> GetFOV(OpenXR::EyeSide side, long frameIdx = -1) const {
        if (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) {
            return m_renderFrames[frameIdx].views->at(side).fov;
        }
        auto views = CurrentViews();
        if (!views.has_value()) return std::nullopt;
        return views->at(side).fov;
    }

    std::optional<XrPosef> GetPose(OpenXR::EyeSide side, long frameIdx = -1) const {
        if (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) {
            return m_renderFrames[frameIdx].views->at(side).pose;
        }
        auto views = CurrentViews();
        if (!views.has_value()) return std::nullopt;
        return views->at(side).pose;
    }

    std::optional<glm::fmat4> GetPoseAsMatrix(OpenXR::EyeSide side, long frameIdx = -1) const {
        auto poseOpt = GetPose(side, frameIdx);
        if (!poseOpt.has_value()) return std::nullopt;
        return ToMat4(ToGLM(poseOpt->position), ToGLM(poseOpt->orientation));
    };

    std::optional<glm::fmat4> GetMiddlePose(long frameIdx = -1) const {
        auto poses = GetPoses(frameIdx);
        if (!poses.has_value()) return std::nullopt;
        const XrPosef& leftPose = poses->at(OpenXR::EyeSide::LEFT).pose;
        const XrPosef& rightPose = poses->at(OpenXR::EyeSide::RIGHT).pose;
        glm::fvec3 middlePos = (ToGLM(leftPose.position) + ToGLM(rightPose.position)) * 0.5f;
        glm::quat middleOri = glm::slerp(ToGLM(leftPose.orientation), ToGLM(rightPose.orientation), 0.5f);

        return ToMat4(middlePos, middleOri);
    };

    double GetLastFrameWorkTimeMs() const { return m_lastFrameWorkTimeMs.load(std::memory_order_relaxed); }
    double GetLastWaitTimeMs() const { return m_lastWaitTimeMs.load(std::memory_order_relaxed); }
    double GetLastFrameTimeMs() const { return m_lastFrameTimeMs.load(std::memory_order_relaxed); }
    double GetPredictedDisplayPeriodMs() const { return m_predictedDisplayPeriodMs.load(std::memory_order_relaxed); }
    double GetLastOverheadMs() const { return m_lastOverheadMs.load(std::memory_order_relaxed); }

    void On3DColorCopied(OpenXR::EyeSide side, long frameIdx) {
        m_renderFrames[frameIdx].copiedColor[side] = true;
        if (!m_renderFrames[frameIdx].views.has_value()) {
#ifndef _WIN32
            std::shared_lock lock(m_viewsMutex);
#endif
            m_renderFrames[frameIdx].views = m_currViews;
        }
        DebugDraw::instance().SnapshotEyeState(side, frameIdx);
    }

    void On3DDepthCopied(OpenXR::EyeSide side, long frameIdx) {
        m_renderFrames[frameIdx].copiedDepth[side] = true;
        if (!m_renderFrames[frameIdx].views.has_value()) {
#ifndef _WIN32
            std::shared_lock lock(m_viewsMutex);
#endif
            m_renderFrames[frameIdx].views = m_currViews;
        }
    }

    void On2DCopied(long frameIdx) {
        m_renderFrames[frameIdx].copied2D = true;
    }

    RenderFrame& GetFrame(long frameIdx) { return m_renderFrames[frameIdx]; }
    const RenderFrame& GetFrame(long frameIdx) const { return m_renderFrames[frameIdx]; }

    class ImGuiOverlay {
    public:
        explicit ImGuiOverlay(VkCommandBuffer cb,VkExtent2D fbRes, VkFormat framebufferFormat);
        ~ImGuiOverlay();

        bool ShouldBlockGameInput() { return ImGui::GetIO().WantCaptureKeyboard; }

        void Update();
        static void Draw3DLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, float aspectRatio, const RenderUtils::UvTransform& uvTransform, long frameIdx);
        static void DrawHUDLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, long frameIdx);
        void Render(long frameIdx, bool renderBackground, bool isDesktopView);
        void DrawAndCopyToImage(VkCommandBuffer cb, VkImage destImage, long frameIdx, bool isDesktopView);
        void DrawHelpMenu();
        void ProcessInputs(OpenXR::InputState& inputs, const VPADStatus& vpadStatus);
        int GetHelpImagePagesCount() const { return m_helpImages.size(); };

    private:
        void DrawSettingsTab(const ImVec2& windowWidth, bool* changed);
        void DrawDebugTab(bool* changed);
        void DrawHelpGuideTab();
        void DrawFPSOverlayTab(const ImVec2& windowWidth, bool* changed);
        void DrawCreditsTab();
        void DrawCustomAttackSensitivityTab(const ImVec2& windowWidth, bool* changed);

        VkDescriptorPool m_descriptorPool;
        VkRenderPass m_renderPass;
        struct HelpImage {
            const char* m_title = nullptr;
            VulkanTexture* m_image;
            VkDescriptorSet m_imageDS = VK_NULL_HANDLE;
        };
        std::vector<HelpImage> m_helpImages;
        uint32_t m_currentHelpImage = 0;

        VkSampler m_sampler = VK_NULL_HANDLE;
        VkExtent2D m_outputRes = {};
    };

    std::unique_ptr<ImGuiOverlay> m_imguiOverlay;
    float m_gameRenderAspectRatio = 16.0f / 9.0f;

    bool IsRendering3D(long frameIdx) {
        return m_renderFrames[frameIdx].presented3D;
    }
    bool IsRendering2D() {
        return m_presented2DLastFrame;
    }
    bool IsInitialized() {
        return m_isInitialized;
    }
    bool IsGameCapturing3DFrameBuffer() const {
        return m_cameraIsCapturing3DFrameBuffer > 0;
    }
    void SignalGameCapturing3DFrameBuffer() {
        m_cameraIsCapturing3DFrameBuffer = 1;
    }

    void SetCustomFadeAmount(float amount) {
        m_customFadeAmount.store(glm::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void SetCustomFadeColor(const glm::fvec3& color) {
        m_customFadeColorR.store(glm::clamp(color.x, 0.0f, 1.0f), std::memory_order_relaxed);
        m_customFadeColorG.store(glm::clamp(color.y, 0.0f, 1.0f), std::memory_order_relaxed);
        m_customFadeColorB.store(glm::clamp(color.z, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    struct CustomFade {
        float amount = 0.0f;
        glm::fvec3 color = glm::fvec3(0.0f);
    };

    CustomFade GetCustomFade() const {
        return {
            .amount = m_customFadeAmount.load(std::memory_order_relaxed),
            .color = {
                m_customFadeColorR.load(std::memory_order_relaxed),
                m_customFadeColorG.load(std::memory_order_relaxed),
                m_customFadeColorB.load(std::memory_order_relaxed),
            },
        };
    }

    bool IsFadeActive() const { return m_isFadeActive.load(std::memory_order_relaxed); }

protected:
    XrSession m_session;
    XrFrameState m_frameState = { XR_TYPE_FRAME_STATE };
    mutable std::optional<std::array<XrView, 2>> m_currViews;
    std::array<RenderFrame, 2> m_renderFrames;

    std::atomic_bool m_isInitialized = false;
    std::atomic_bool m_presented2DLastFrame = false;
#ifndef _WIN32
#include "renderer_state_vulkan.inc"
#endif
    std::atomic_uint8_t m_cameraIsCapturing3DFrameBuffer = 0;
    std::atomic_bool m_isFadeActive = false;
    std::atomic<float> m_customFadeAmount = 0.0f;
    std::atomic<float> m_customFadeColorR = 0.0f;
    std::atomic<float> m_customFadeColorG = 0.0f;
    std::atomic<float> m_customFadeColorB = 0.0f;

    // Full-frame timing derived from OpenXR timestamps (XrTime is in nanoseconds)
    std::atomic<XrTime> m_lastPredictedDisplayTime = 0;

    std::chrono::high_resolution_clock::time_point m_frameStartTime;

    std::atomic<double> m_lastFrameWorkTimeMs = 0.0;
    std::atomic<double> m_lastWaitTimeMs = 0.0;

    // Derived from OpenXR timestamps
    std::atomic<double> m_lastFrameTimeMs = 0.0;
    std::atomic<double> m_predictedDisplayPeriodMs = 0.0;
    std::atomic<double> m_lastOverheadMs = 0.0;
};
