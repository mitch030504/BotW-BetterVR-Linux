#include "pch.h"
#ifndef _WIN32

#include "renderer.h"
#include "instance.h"
#include "rendering/linux_desktop_mirror.h"
#include "utils/vulkan_utils.h"
#include "hooking/framebuffer_linux_diag.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>

static bool LinuxEnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return false;
    return std::strcmp(value, "0") != 0
        && std::strcmp(value, "false") != 0
        && std::strcmp(value, "FALSE") != 0
        && std::strcmp(value, "no") != 0
        && std::strcmp(value, "NO") != 0;
}

// Linux Vulkan swapchain wrapper for OpenXR
struct VkSwapchainWrapper {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t currentImageIdx = 0;
    bool acquired = false;

    void Create(XrSession session, uint32_t w, uint32_t h, VkFormat format, XrSwapchainUsageFlags usage) {
        width = w;
        height = h;

        XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = usage;
        createInfo.format = format;
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        checkXRResult(xrCreateSwapchain(session, &createInfo, &handle), "Failed to create Vulkan OpenXR swapchain!");
        Log::print<INFO>("Created OpenXR Vulkan swapchain {}x{} format={}", width, height, (int)format);

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(handle, 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageVulkanKHR> xrImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
        xrEnumerateSwapchainImages(handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));

        images.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; i++) {
            images[i] = xrImages[i].image;
        }
        Log::print<INFO>("  Swapchain has {} images", imageCount);
    }

    VkImage Acquire() {
        XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        xrAcquireSwapchainImage(handle, &acquireInfo, &currentImageIdx);

        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(handle, &waitInfo);

        acquired = true;
        return images[currentImageIdx];
    }

    void Release() {
        if (acquired) {
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(handle, &releaseInfo);
            acquired = false;
        }
    }

    void Destroy() {
        if (handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(handle);
            handle = XR_NULL_HANDLE;
        }
    }
};

// Per-platform data stored in the renderer
struct LinuxRenderState {
    std::array<VkSwapchainWrapper, 2> colorSwapchains; // LEFT, RIGHT
    std::array<VkSwapchainWrapper, 2> depthSwapchains; // LEFT, RIGHT
    VkSwapchainWrapper hudSwapchain; // 2D HUD layer

    // Double-buffered intermediate textures: Cemu blits 3D into the encoded
    // magic-frame slot and HUD into its active write index. Submit thread reads
    // from latest.readBufferIdx. This avoids a single-intermediate race when
    // Cemu starts writing the next frame while submit thread is still reading
    // previous content.
    // First index is eye (or unused for HUD), second is buffer (0 or 1).
    VulkanTexture* intermediateColor[2][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    VulkanTexture* intermediateDepth[2][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    VulkanTexture* intermediateHud[2] = { nullptr, nullptr };
    struct Captured3DFrame {
        bool eyeCopied[2] = { false, false };
        bool depthCopied[2] = { false, false };
        bool complete = false;
        bool hasRenderViews = false;
        std::array<XrView, 2> renderViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
        XrTime renderDisplayTime = 0;
        std::array<std::chrono::high_resolution_clock::time_point, 2> colorCopyAt = {};

        void Reset() {
            eyeCopied[0] = false;
            eyeCopied[1] = false;
            depthCopied[0] = false;
            depthCopied[1] = false;
            complete = false;
            hasRenderViews = false;
            renderViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
            renderDisplayTime = 0;
            colorCopyAt[0] = {};
            colorCopyAt[1] = {};
        }
    };

    struct Captured3DPair {
        bool eyeCopied[2] = { false, false };
        int bufferIdx[2] = { -1, -1 };
        bool hasRenderViews = false;
        std::array<XrView, 2> renderViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
        XrTime renderDisplayTime = 0;
        std::array<std::chrono::high_resolution_clock::time_point, 2> colorCopyAt = {};

        void Reset() {
            eyeCopied[0] = false;
            eyeCopied[1] = false;
            bufferIdx[0] = -1;
            bufferIdx[1] = -1;
            hasRenderViews = false;
            renderViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
            renderDisplayTime = 0;
            colorCopyAt[0] = {};
            colorCopyAt[1] = {};
        }

        bool IsComplete() const {
            return eyeCopied[0] && eyeCopied[1] && bufferIdx[0] >= 0 && bufferIdx[1] >= 0;
        }
    };

    // BotW encodes a 0/1 stereo frame index in the magic clear values. Use that
    // as the physical intermediate slot for each eye, but publish stereo content
    // as the observed left-to-right pair. The main pause menu can perturb the
    // encoded slot cadence; carrying a per-eye slot pair avoids publishing a
    // fresh left eye with a stale right eye after the menu closes.
    std::array<Captured3DFrame, 2> captured3DFrames;
    Captured3DPair pending3DPair;
    Captured3DPair completed3DPair;

    int currentWriteIdx_hud = 0; // Cemu writes intermediateHud here

    std::array<XrCompositionLayerProjectionView, 2> projectionViews = {};
    std::array<XrCompositionLayerDepthInfoKHR, 2> depthInfos = {};
    bool swapchainCreated = false;
    bool frameHasContent = false;
    bool hudCopied = false;
    bool disableDepthReprojection = false;
    bool frameTimingStatsEnabled = false;
    bool captureDiagnosticsEnabled = false;

    struct PauseDiagnostics {
        std::atomic<uint64_t> epoch{0};
        std::atomic_bool inInventoryPause{false};
        std::atomic<int> verbose3DCopiesRemaining{0};
    } pauseDiag;

    // Command resources for intermediate→swapchain copy in EndFrame
    VkCommandPool copyPool = VK_NULL_HANDLE;
    VkCommandBuffer copyCmdBuf = VK_NULL_HANDLE;
    VkFence copyFence = VK_NULL_HANDLE;

    // Phase 2: Async submit thread infrastructure (idle until Phase 3+ wires it up).
    // The submit thread will eventually own all VkQueue submissions for VR work
    // (using the dedicated queue from Phase 1) and own the OpenXR frame lifecycle
    // (xrEndFrame, xrWaitFrame, etc. in Phase 5). For now we just create the
    // resources; no work is moved to the thread yet.
    struct SubmitInfrastructure {
        VkQueue submitQueue = VK_NULL_HANDLE;        // queue 1 from Phase 1 (or queue 0 if no dedicated)
        bool hasDedicatedQueue = false;

        // Timeline semaphore for queue 0 → queue 1 sync (Phase 4).
        // Cemu's queue 0 signals after writing to intermediates; our submit on
        // queue 1 waits before reading them.
        VkSemaphore intermediateReady = VK_NULL_HANDLE;
        std::atomic<uint64_t> nextSignalValue{1};

        // Work-item queue: Cemu thread hands off frame-submission descriptors here.
        // Each WorkItem is self-contained: layer descriptors are copied by value
        // and pointer chains (depth-info, layers array) are fixed up on the submit
        // thread right before xrEndFrame. The work item is destroyed after that.
        struct WorkItem {
            bool stop = false;
            XrTime displayTime = 0;
            XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            bool has3D = false;
            bool hasHud = false;
            bool hasDepth = false;
            XrCompositionLayerProjection projectionLayer{};
            std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
            std::array<XrCompositionLayerDepthInfoKHR, 2> depthInfos{};
            XrCompositionLayerQuad hudLayer{};
        };
        std::mutex workMutex;
        std::condition_variable workCv;
        std::queue<WorkItem> workQueue;
        std::atomic_bool stopRequested{false};
        std::thread thread;

        // Completion signal: submit thread sets `previousFrameDone = true` after
        // xrEndFrame returns. StartFrame on Cemu thread waits on this before
        // calling xrWaitFrame (OpenXR requires sequential End → Wait).
        std::mutex completionMutex;
        std::condition_variable completionCv;
        bool previousFrameDone = true; // start true so the very first frame doesn't wait

        // Phase 5+6: pose pipeline. Submit thread now owns the entire OpenXR
        // frame lifecycle (xrEndFrame → xrWaitFrame → UpdateViews) and stores
        // the result here. Cemu's camera hooks atomically claim the latch and
        // copy pendingViews → m_currViews on the first read of each frame.
        std::mutex pendingMutex;
        std::condition_variable pendingCv;
        std::optional<std::array<XrView, 2>> pendingViews;
        XrFrameState pendingFrameState = { XR_TYPE_FRAME_STATE };
        std::atomic<uint64_t> pendingFrameId{0};   // incremented by submit thread after preparing pendingViews

        // Phase 7: latest content state from Cemu. Cemu's QueuePresentKHR updates
        // this each time intermediates are filled with new game content. The
        // submit thread reads this each iteration to know what to copy and how
        // to build layers. intermediateReadyValue is the timeline semaphore value
        // Cemu has signaled on queue 0; submit thread's copy on queue 1 waits on
        // this for cross-queue memory visibility.
        struct LatestContent {
            uint64_t intermediateReadyValue = 0;
            // Per-layer read buffer indices. Only updated when that specific
            // layer's content was written in this Cemu push, so submit thread
            // always reads from a buffer that has been written to.
            int readBufferIdx_3D[2] = { 0, 0 };
            int readBufferIdx_hud = 0;
            bool has3D = false;
            bool hasHud = false;
            bool hasDepth = false;
            bool eyeCopied[2] = { false, false };
            bool depthCopied[2] = { false, false };
            bool hudCopied = false;
            float customFadeAmount = 0.0f;
            bool fadeActive = false;
            uint64_t contentSeq = 0;
            std::chrono::high_resolution_clock::time_point publishedAt;
            std::array<XrView, 2> renderViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
            XrTime renderDisplayTime = 0;
            bool hasRenderViews = false;
        };
        std::mutex latestMutex;
        LatestContent latest;

        // Diagnostic markers for stall investigation. Submit thread updates
        // currentStep at each major operation; LatchPendingIfNeeded reports
        // the last seen step in its timeout log.
        std::atomic<uint64_t> iterCount{0};
        std::atomic<int> currentStep{0};
        // Set to false by submit thread when it exits (fatal error). Cemu's
        // LatchPendingIfNeeded checks this and skips waiting (proceeds with
        // stale m_currViews) instead of timing out every frame.
        std::atomic<bool> alive{false};
    } submit;

    // Frame timing stats: rolling window of measurements + periodic log summary.
    // All durations in milliseconds (double for sub-microsecond precision).
    struct FrameStats {
        struct Window {
            static constexpr size_t SIZE = 120; // ~2s at 60Hz
            mutable std::mutex mutex;
            std::array<double, SIZE> samples{};
            size_t count = 0;
            size_t writeIdx = 0;
            void Add(double v) {
                std::lock_guard<std::mutex> lock(mutex);
                samples[writeIdx] = v;
                writeIdx = (writeIdx + 1) % SIZE;
                if (count < SIZE) count++;
            }
            struct Summary { double min, p50, p99, max, mean; };
            Summary Compute() const {
                std::lock_guard<std::mutex> lock(mutex);
                if (count == 0) return {0, 0, 0, 0, 0};
                std::vector<double> sorted(samples.begin(), samples.begin() + count);
                std::sort(sorted.begin(), sorted.end());
                double sum = 0;
                for (double v : sorted) sum += v;
                size_t p50Idx = count / 2;
                size_t p99Idx = std::min(count - 1, (size_t)(count * 0.99));
                return { sorted.front(), sorted[p50Idx], sorted[p99Idx], sorted.back(), sum / count };
            }
        };

        Window waitFrame;       // xrWaitFrame block time on submit thread
        Window beginFrame;      // xrBeginFrame call on submit thread
        Window locateViews;     // submit-thread xrLocateViews for Cemu camera latch
        Window lateLocateViews; // just-before-xrEndFrame xrLocateViews for layer poses
        Window updateActions;   // OpenXR action polling and controller pose update
        Window waitBeginUpdate; // total xrWaitFrame -> xrBeginFrame -> locate/actions
        Window startFrame;      // total StartFrame work on Cemu thread
        Window cemuWork;        // StartFrame exit → EndFrame entry (game render path)
        Window endFrame;        // EndFrame total on Cemu thread
        Window cemuSignal;      // Cemu queue signal-only vkQueueSubmit
        Window submitInterval;  // wall-clock between submit-thread iterations
        Window acquireSwapchains;
        Window recordCopies;
        Window queueSubmitCopy;
        Window waitCopyFence;
        Window releaseSwapchains;
        Window composeLayers;   // late-latch + composition-layer construction
        Window xrEndFrame;      // xrEndFrame call itself
        Window poseAge;         // (now - lastPoseSampleTime) at xrEndFrame
        Window poseTargetDelta; // pose sample target display time - submitted display time
        Window contentAge;      // (now - Cemu content publish time) at xrEndFrame
        Window contentPoseDelta; // content render display time - submitted display time
        Window eyeCaptureDelta; // right-eye accepted color copy time - left-eye accepted color copy time
        Window frameInterval;   // wall-clock between consecutive QueuePresentKHR

        std::chrono::high_resolution_clock::time_point startFrameExitTime;
        std::chrono::high_resolution_clock::time_point endFrameEntryTime;
        std::chrono::high_resolution_clock::time_point lastPresentTime;
        std::chrono::high_resolution_clock::time_point lastSubmitLoopTime;
        std::chrono::steady_clock::time_point lastStatsLogTime;
        uint64_t framesLogged = 0;
        static constexpr uint64_t LOG_EVERY_N_FRAMES = 120;

        std::atomic<uint64_t> submitFrames{0};
        std::atomic<uint64_t> submitFramesWithLayers{0};
        std::atomic<uint64_t> submitFramesWithCopy{0};
        std::atomic<uint64_t> submitFramesWithNewContent{0};
        std::atomic<uint64_t> submitFramesReusingContent{0};
        std::atomic<uint64_t> submitFramesNoContent{0};
        std::atomic<uint64_t> cemuPublishes{0};
        std::atomic<uint64_t> cemuPublishes3D{0};
        std::atomic<uint64_t> cemuPublishesHud{0};
        std::atomic<uint64_t> cemuSignalFailures{0};
        std::atomic<uint64_t> latePoseLocateFailures{0};
        std::atomic<uint64_t> projectionFramesUsingContentPose{0};
        std::atomic<uint64_t> projectionFramesFreshPoseFallback{0};
        std::array<std::atomic<uint64_t>, 2> diag3DColorCopies{};
        std::array<std::atomic<uint64_t>, 2> diag3DDepthCopies{};
        std::atomic<uint64_t> diag3DPendingLeftStarts{0};
        std::atomic<uint64_t> diag3DCompletedPairs{0};
        std::atomic<uint64_t> diag3DOrphanRightDrops{0};
    } stats;

    // Dedicated head-pose-sampling thread: continuously calls xrLocateViews so
    // the submit thread has a fallback source if its final late-locate fails.
    struct PoseTracking {
        std::thread thread;
        std::atomic_bool running{false};

        // Reference for the thread to estimate predicted display times: the
        // most recent xrWaitFrame predictedDisplayTime, and the wall-clock time
        // when we received it. Future targets = baseDisplayTime + (now - baseSampleTime).
        std::atomic<XrTime> baseDisplayTime{0};
        std::atomic<XrDuration> displayPeriod{16'666'666}; // ~60Hz default
        std::mutex refMutex;
        std::chrono::steady_clock::time_point baseSampleTime;

        // Latest pose sample (raw, no height offset). Protected by viewsMutex.
        std::mutex viewsMutex;
        std::optional<std::array<XrView, 2>> latestRawViews;
        std::chrono::high_resolution_clock::time_point latestSampleWallTime;
        XrTime latestSampleTargetTime = 0;
        std::atomic<uint64_t> sampleCount{0};
        std::atomic<uint64_t> failureCount{0};
    } poseTracking;

    void StopPoseThread() {
        if (poseTracking.running.exchange(false)) {
            if (poseTracking.thread.joinable()) {
                poseTracking.thread.join();
            }
        }
    }

    void StopSubmitThread() {
        {
            std::lock_guard<std::mutex> lock(submit.workMutex);
            submit.stopRequested.store(true);
        }
        submit.workCv.notify_all();
        if (submit.thread.joinable()) {
            submit.thread.join();
        }
    }

    ~LinuxRenderState() {
        StopPoseThread();
        StopSubmitThread();
        if (submit.intermediateReady != VK_NULL_HANDLE) {
            auto* d = VRManager::instance().VK ? VRManager::instance().VK->GetDeviceDispatch() : nullptr;
            auto dev = VRManager::instance().VK ? VRManager::instance().VK->GetDevice() : VK_NULL_HANDLE;
            if (d && dev) d->DestroySemaphore(dev, submit.intermediateReady, nullptr);
        }
        auto* dispatch = VRManager::instance().VK ? VRManager::instance().VK->GetDeviceDispatch() : nullptr;
        auto device = VRManager::instance().VK ? VRManager::instance().VK->GetDevice() : VK_NULL_HANDLE;
        if (dispatch && device) {
            if (copyFence) dispatch->DestroyFence(device, copyFence, nullptr);
            if (copyPool) dispatch->DestroyCommandPool(device, copyPool, nullptr);
        }
        for (int e = 0; e < 2; e++) {
            for (int b = 0; b < 2; b++) {
                delete intermediateColor[e][b]; intermediateColor[e][b] = nullptr;
                delete intermediateDepth[e][b]; intermediateDepth[e][b] = nullptr;
            }
        }
        delete intermediateHud[0]; intermediateHud[0] = nullptr;
        delete intermediateHud[1]; intermediateHud[1] = nullptr;
        for (auto& sc : colorSwapchains) sc.Destroy();
        for (auto& sc : depthSwapchains) sc.Destroy();
        hudSwapchain.Destroy();
    }
};

static std::unique_ptr<LinuxRenderState> s_linux;

static double LinuxDisplayTimeDeltaMs(XrTime value, XrTime reference) {
    if (value == 0 || reference == 0) return 0.0;
    return (double)(value - reference) / 1e6;
}

void RND_Renderer::LinuxMarkInventoryPauseTransition(bool opened) {
    if (!s_linux) return;

    uint64_t epoch = opened
        ? s_linux->pauseDiag.epoch.fetch_add(1, std::memory_order_acq_rel) + 1
        : s_linux->pauseDiag.epoch.load(std::memory_order_acquire);
    s_linux->pauseDiag.inInventoryPause.store(opened, std::memory_order_release);
    s_linux->pauseDiag.verbose3DCopiesRemaining.store(
        s_linux->captureDiagnosticsEnabled ? 24 : 0,
        std::memory_order_release);

    if (!s_linux->captureDiagnosticsEnabled) return;

    XrTime currViewsDisplayTime = 0;
    {
        std::shared_lock viewsLock(m_viewsMutex);
        currViewsDisplayTime = m_linuxCurrViewsDisplayTime;
    }

    LinuxRenderState::SubmitInfrastructure::LatestContent latest;
    {
        std::lock_guard<std::mutex> lock(s_linux->submit.latestMutex);
        latest = s_linux->submit.latest;
    }

    const auto pending = s_linux->pending3DPair;
    const auto completed = s_linux->completed3DPair;
    const auto slot0 = s_linux->captured3DFrames[0];
    const auto slot1 = s_linux->captured3DFrames[1];

    Log::print<INFO>(
        "Linux pause diag {} epoch={} currDt={} latest seq={} gate={} has3D={} hasHud={} hasDepth={} idx=({}, {}) eye=({}, {}) depth=({}, {}) renderDelta={:.2f}ms | pending eye=({}, {}) idx=({}, {}) pose={} renderDelta={:.2f}ms | completed eye=({}, {}) idx=({}, {}) pose={} renderDelta={:.2f}ms frameHasContent={} | slot0 eye=({}, {}) depth=({}, {}) complete={} pose={} | slot1 eye=({}, {}) depth=({}, {}) complete={} pose={}",
        opened ? "opened" : "closed",
        epoch,
        currViewsDisplayTime,
        latest.contentSeq,
        latest.intermediateReadyValue,
        latest.has3D ? "yes" : "no",
        latest.hasHud ? "yes" : "no",
        latest.hasDepth ? "yes" : "no",
        latest.readBufferIdx_3D[0], latest.readBufferIdx_3D[1],
        latest.eyeCopied[0] ? "yes" : "no", latest.eyeCopied[1] ? "yes" : "no",
        latest.depthCopied[0] ? "yes" : "no", latest.depthCopied[1] ? "yes" : "no",
        LinuxDisplayTimeDeltaMs(latest.renderDisplayTime, currViewsDisplayTime),
        pending.eyeCopied[0] ? "yes" : "no", pending.eyeCopied[1] ? "yes" : "no",
        pending.bufferIdx[0], pending.bufferIdx[1],
        pending.hasRenderViews ? "yes" : "no",
        LinuxDisplayTimeDeltaMs(pending.renderDisplayTime, currViewsDisplayTime),
        completed.eyeCopied[0] ? "yes" : "no", completed.eyeCopied[1] ? "yes" : "no",
        completed.bufferIdx[0], completed.bufferIdx[1],
        completed.hasRenderViews ? "yes" : "no",
        LinuxDisplayTimeDeltaMs(completed.renderDisplayTime, currViewsDisplayTime),
        s_linux->frameHasContent ? "yes" : "no",
        slot0.eyeCopied[0] ? "yes" : "no", slot0.eyeCopied[1] ? "yes" : "no",
        slot0.depthCopied[0] ? "yes" : "no", slot0.depthCopied[1] ? "yes" : "no",
        slot0.complete ? "yes" : "no",
        slot0.hasRenderViews ? "yes" : "no",
        slot1.eyeCopied[0] ? "yes" : "no", slot1.eyeCopied[1] ? "yes" : "no",
        slot1.depthCopied[0] ? "yes" : "no", slot1.depthCopied[1] ? "yes" : "no",
        slot1.complete ? "yes" : "no",
        slot1.hasRenderViews ? "yes" : "no");
}

void RND_Renderer::LinuxUpdateRuntimeFrameTiming(const XrFrameState& frameState, double waitFrameMs) {
    m_lastWaitTimeMs.store(waitFrameMs, std::memory_order_relaxed);

    if (frameState.predictedDisplayPeriod > 0) {
        m_predictedDisplayPeriodMs.store((double)frameState.predictedDisplayPeriod / 1e6,
                                         std::memory_order_relaxed);
    }

    if (frameState.predictedDisplayTime == 0) return;

    const XrTime previousDisplayTime =
        m_lastPredictedDisplayTime.exchange(frameState.predictedDisplayTime, std::memory_order_relaxed);
    if (previousDisplayTime != 0 && frameState.predictedDisplayTime > previousDisplayTime) {
        const XrTime deltaNs = frameState.predictedDisplayTime - previousDisplayTime;
        const double frameTimeMs = (double)deltaNs / 1e6;
        const double periodMs = frameState.predictedDisplayPeriod > 0
            ? (double)frameState.predictedDisplayPeriod / 1e6
            : m_predictedDisplayPeriodMs.load(std::memory_order_relaxed);
        const double overheadMs = frameTimeMs - periodMs;
        m_lastFrameTimeMs.store(frameTimeMs, std::memory_order_relaxed);
        m_lastOverheadMs.store(overheadMs > 0.0 ? overheadMs : 0.0, std::memory_order_relaxed);
    }
}

RND_Renderer::RND_Renderer(XrSession xrSession): m_session(xrSession) {
    Log::print<INFO>("Linux RND_Renderer: Initializing...");

    s_linux = std::make_unique<LinuxRenderState>();
    s_linux->frameTimingStatsEnabled =
        LinuxEnvFlagEnabled("BETTERVR_LINUX_FRAME_STATS")
        || LinuxEnvFlagEnabled("BETTERVR_LINUX_TIMING_STATS");
    s_linux->captureDiagnosticsEnabled = LinuxEnvFlagEnabled("BETTERVR_LINUX_CAPTURE_DIAG");
    s_linux->disableDepthReprojection =
        LinuxEnvFlagEnabled("BETTERVR_LINUX_DISABLE_DEPTH_REPROJECTION")
        || LinuxEnvFlagEnabled("BETTERVR_LINUX_DISABLE_DEPTH");
    if (s_linux->frameTimingStatsEnabled) {
        Log::print<INFO>("Linux frame timing stats enabled (BETTERVR_LINUX_FRAME_STATS)");
    }
    if (s_linux->captureDiagnosticsEnabled) {
        Log::print<INFO>("Linux capture diagnostics enabled (BETTERVR_LINUX_CAPTURE_DIAG)");
    }
    if (s_linux->disableDepthReprojection) {
        Log::print<WARNING>("Linux OpenXR depth composition disabled by environment for diagnostics");
    }

    // Begin the XR session
    XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrResult result = xrBeginSession(m_session, &beginInfo);
    if (XR_FAILED(result)) {
        Log::print<ERROR>("Failed to begin OpenXR session: {}", result);
        return;
    }
    Log::print<INFO>("OpenXR session begun successfully!");

    // Create swapchains
    auto viewConfs = VRManager::instance().XR->GetViewConfigurations();
    for (int eye = 0; eye < 2; eye++) {
        s_linux->colorSwapchains[eye].Create(
            m_session,
            viewConfs[eye].recommendedImageRectWidth,
            viewConfs[eye].recommendedImageRectHeight,
            VK_FORMAT_R8G8B8A8_SRGB,
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
        );
        s_linux->depthSwapchains[eye].Create(
            m_session,
            viewConfs[eye].recommendedImageRectWidth,
            viewConfs[eye].recommendedImageRectHeight,
            VK_FORMAT_D32_SFLOAT,
            XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
        );
    }
    s_linux->swapchainCreated = true;

    // Create DOUBLE-BUFFERED intermediate textures matching swapchain resolution.
    // Two of each so Cemu writes to one while submit thread reads the other.
    for (int eye = 0; eye < 2; eye++) {
        uint32_t w = viewConfs[eye].recommendedImageRectWidth;
        uint32_t h = viewConfs[eye].recommendedImageRectHeight;
        for (int b = 0; b < 2; b++) {
            s_linux->intermediateColor[eye][b] = new VulkanTexture(w, h,
                VK_FORMAT_R8G8B8A8_SRGB,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            s_linux->intermediateDepth[eye][b] = new VulkanTexture(w, h,
                VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
        Log::print<INFO>("Created double-buffered intermediate textures eye={}: {}x{} (2x)", eye, w, h);
    }

    // Create command pool/buffer/fence for intermediate→swapchain copies
    {
        auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
        auto device = VRManager::instance().VK->GetDevice();

        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = VRManager::instance().VK->GetQueueFamilyIndex();
        dispatch->CreateCommandPool(device, &poolInfo, nullptr, &s_linux->copyPool);

        VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = s_linux->copyPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        dispatch->AllocateCommandBuffers(device, &allocInfo, &s_linux->copyCmdBuf);

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        dispatch->CreateFence(device, &fenceInfo, nullptr, &s_linux->copyFence);
    }

    // Phase 2 / 3-queue topology: set up submit infrastructure. Submit thread
    // uses the COPY queue (queue 2) if available; falls back to the runtime
    // queue (queue 1, shared with OpenXR) if only one extra queue was allocated;
    // falls back to Cemu's queue (queue 0) if no extras.
    {
        auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
        auto device = VRManager::instance().VK->GetDevice();
        uint32_t famIdx = VRManager::instance().VK->GetQueueFamilyIndex();
        int32_t copyIdx = VRManager::instance().VK->GetCopyQueueIndex();
        int32_t submitIdx = VRManager::instance().VK->GetSubmitQueueIndex();
        uint32_t chosenIdx = 0;
        const char* topology = "SHARED WITH CEMU";
        if (copyIdx >= 0) {
            chosenIdx = (uint32_t)copyIdx;
            topology = "DEDICATED COPY QUEUE (queue 2)";
        } else if (submitIdx >= 0) {
            chosenIdx = (uint32_t)submitIdx;
            topology = "SHARED WITH RUNTIME (queue 1)";
        } else {
            chosenIdx = VRManager::instance().VK->GetQueueIndex();
        }
        dispatch->GetDeviceQueue(device, famIdx, chosenIdx, &s_linux->submit.submitQueue);
        s_linux->submit.hasDedicatedQueue = (copyIdx >= 0);
        Log::print<INFO>("Submit thread queue: family={} index={} ({})",
            famIdx, chosenIdx, topology);

        // Timeline semaphore for queue 0 → queue 1 sync (used in Phase 4).
        VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;
        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semInfo.pNext = &timelineInfo;
        VkResult semRes = dispatch->CreateSemaphore(device, &semInfo, nullptr, &s_linux->submit.intermediateReady);
        if (semRes != VK_SUCCESS) {
            Log::print<ERROR>("Failed to create timeline semaphore for queue sync: {}", (int)semRes);
            s_linux->submit.intermediateReady = VK_NULL_HANDLE;
        }
    }

    m_isInitialized = true;
    Log::print<INFO>("Linux RND_Renderer: Initialized with stereo swapchains + intermediates");
}

RND_Renderer::~RND_Renderer() {
    s_linux.reset();
    if (m_session != XR_NULL_HANDLE) {
        xrEndSession(m_session);
    }
    Log::print<INFO>("Linux RND_Renderer: Destroyed");
}

// Head-pose-sampling thread: runs at ~250 Hz, continuously calling xrLocateViews
// as a fallback source for submit-time composition. Only touches OpenXR read-only calls;
// never accesses the Vulkan queue. The OpenXR spec permits concurrent xrLocateViews
// calls from multiple threads. We sample using a target time projected forward
// from the most recent xrWaitFrame's predictedDisplayTime so the prediction window
// matches what xrEndFrame will actually consume.
static void PoseTrackingThreadMain(XrSession session, XrSpace stageSpace) {
    Log::print<INFO>("Head pose tracking thread: started");
    if (stageSpace == XR_NULL_HANDLE) {
        Log::print<ERROR>("Pose thread: no stage space available; aborting");
        return;
    }

    while (s_linux && s_linux->poseTracking.running.load(std::memory_order_acquire)) {
        // Compute target display time: extrapolate forward from the last xrWaitFrame.
        // For a future predicted display time, we want the NEXT frame boundary, not "now".
        XrTime target;
        {
            std::lock_guard<std::mutex> lock(s_linux->poseTracking.refMutex);
            XrTime base = s_linux->poseTracking.baseDisplayTime.load(std::memory_order_relaxed);
            if (base == 0) {
                target = 0;
            } else {
                auto elapsed = std::chrono::steady_clock::now() - s_linux->poseTracking.baseSampleTime;
                auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                XrDuration period = s_linux->poseTracking.displayPeriod.load(std::memory_order_relaxed);
                if (period <= 0) period = 16'666'666;
                // Round up to the next display-time boundary at or after now+(half period).
                // This keeps the predicted target slightly ahead of the moment we sample at,
                // which is what xrEndFrame will consume.
                XrTime futureBoundary = base + ((elapsed_ns / period) + 1) * period;
                target = futureBoundary;
            }
        }
        if (target == 0) {
            // No xrWaitFrame has completed yet; nothing to predict against.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = target;
        locateInfo.space = stageSpace;

        XrViewState viewState = { XR_TYPE_VIEW_STATE };
        uint32_t viewCount = 2;
        std::array<XrView, 2> views = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
        XrResult r = xrLocateViews(session, &locateInfo, &viewState, viewCount, &viewCount, views.data());

        if (XR_SUCCEEDED(r) && viewCount == 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            auto sampleWallTime = std::chrono::high_resolution_clock::now();
            std::lock_guard<std::mutex> lock(s_linux->poseTracking.viewsMutex);
            s_linux->poseTracking.latestRawViews = views;
            s_linux->poseTracking.latestSampleWallTime = sampleWallTime;
            s_linux->poseTracking.latestSampleTargetTime = target;
            s_linux->poseTracking.sampleCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_linux->poseTracking.failureCount.fetch_add(1, std::memory_order_relaxed);
        }

        // ~250 Hz sampling. Display refreshes at 75-90 Hz, so this is ~3x
        // display rate — plenty for late-latching freshness (poseAge stays
        // under ~5ms p99) without flooding SteamVR's runtime with xrLocateViews
        // calls. The previous 1ms sleep (1000+ Hz) was an order of magnitude
        // faster than necessary and likely contributed to NVIDIA driver stress
        // (the SteamVR compositor crash backtrace was hung in CGpuTiming —
        // GPU timing query infrastructure).
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    Log::print<INFO>("Head pose tracking thread: stopped");
}

// Phase 7 submit thread: master clock for VR submission. Loops at native
// vsync (paced by xrWaitFrame). Each iteration:
//   - Reads latest content state from Cemu (via atomic snapshot)
//   - If new content: acquires swapchains, records intermediate→swapchain copies
//     on queue 1 (waits on Cemu's timeline semaphore for cross-queue sync)
//   - Builds composition layers: 3D uses the content render pose, HUD uses the
//     freshest head pose
//   - xrEndFrame
// Between Cemu pushes, the loop keeps running at native vsync, re-submitting
// the same content render pose and depth so SteamVR can reproject it to the
// current display pose. HUD remains late-latched to the freshest pose.
static void SubmitThreadMain(XrSession session, XrSpace stageSpace) {
    Log::print<INFO>("Submit thread: started");
    if (s_linux) s_linux->submit.alive.store(true, std::memory_order_release);

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    auto device = VRManager::instance().VK->GetDevice();
    VkQueue submitQueue = s_linux ? s_linux->submit.submitQueue : VK_NULL_HANDLE;
    if (submitQueue == VK_NULL_HANDLE) {
        Log::print<ERROR>("Submit thread: no queue available; aborting");
        if (s_linux) s_linux->submit.alive.store(false, std::memory_order_release);
        return;
    }

    XrFrameState currentFrameState = { XR_TYPE_FRAME_STATE };
    uint64_t lastCopiedContentSeq = 0;

    // Wait + Begin + UpdateViews + UpdateActions. Used for bootstrap AND to
    // pace each subsequent iteration of the loop (xrWaitFrame blocks until
    // the runtime is ready for the next frame — this is our vsync).
    auto runWaitBeginUpdate = [&]() {
        auto totalStart = std::chrono::high_resolution_clock::now();
        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
        currentFrameState = { XR_TYPE_FRAME_STATE };
        auto waitStart = std::chrono::high_resolution_clock::now();
        XrResult r = xrWaitFrame(session, &waitInfo, &currentFrameState);
        auto waitStop = std::chrono::high_resolution_clock::now();
        double waitFrameMs = std::chrono::duration<double, std::milli>(waitStop - waitStart).count();
        s_linux->stats.waitFrame.Add(waitFrameMs);
        if (XR_FAILED(r)) {
            static int s_failCount = 0;
            if (s_failCount++ < 5) Log::print<ERROR>("Submit thread: xrWaitFrame failed: {}", (int)r);
            return;
        }

        if (auto* renderer = VRManager::instance().XR ? VRManager::instance().XR->GetRenderer() : nullptr) {
            renderer->LinuxUpdateRuntimeFrameTiming(currentFrameState, waitFrameMs);
        }

        if (currentFrameState.predictedDisplayTime != 0) {
            {
                std::lock_guard<std::mutex> lock(s_linux->poseTracking.refMutex);
                s_linux->poseTracking.baseSampleTime = std::chrono::steady_clock::now();
                s_linux->poseTracking.baseDisplayTime.store(currentFrameState.predictedDisplayTime,
                                                            std::memory_order_release);
                if (currentFrameState.predictedDisplayPeriod > 0) {
                    s_linux->poseTracking.displayPeriod.store(currentFrameState.predictedDisplayPeriod,
                                                              std::memory_order_relaxed);
                }
            }
        }

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        auto beginStart = std::chrono::high_resolution_clock::now();
        XrResult br = xrBeginFrame(session, &beginInfo);
        auto beginStop = std::chrono::high_resolution_clock::now();
        s_linux->stats.beginFrame.Add(std::chrono::duration<double, std::milli>(beginStop - beginStart).count());
        if (XR_FAILED(br)) {
            static int s_failCount = 0;
            if (s_failCount++ < 5) Log::print<ERROR>("Submit thread: xrBeginFrame failed: {}", (int)br);
            return;
        }

        static bool s_loggedFirst = false;
        if (!s_loggedFirst) {
            s_loggedFirst = true;
            Log::print<INFO>("Submit thread bootstrap: xrWaitFrame returned predictedDisplayTime={}", currentFrameState.predictedDisplayTime);
        }

        if (currentFrameState.predictedDisplayTime != 0) {
            VRManager::instance().XR->UpdateSpaces(currentFrameState.predictedDisplayTime);

            XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = currentFrameState.predictedDisplayTime;
            locateInfo.space = stageSpace;
            XrViewState viewState = { XR_TYPE_VIEW_STATE };
            uint32_t viewCount = 2;
            std::array<XrView, 2> views = { XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW} };
            auto locateStart = std::chrono::high_resolution_clock::now();
            XrResult lr = xrLocateViews(session, &locateInfo, &viewState, viewCount, &viewCount, views.data());
            auto locateStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.locateViews.Add(std::chrono::duration<double, std::milli>(locateStop - locateStart).count());

            if (XR_SUCCEEDED(lr) && viewCount == 2 &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                {
                    std::lock_guard<std::mutex> lock(s_linux->submit.pendingMutex);
                    s_linux->submit.pendingViews = views;
                    s_linux->submit.pendingFrameState = currentFrameState;
                    s_linux->submit.pendingFrameId.fetch_add(1, std::memory_order_release);
                }
                // Update OpenXR action state (for controllers/buttons).
                XrPosef leftPose = views[0].pose;
                XrPosef rightPose = views[1].pose;
                glm::fvec3 mid = (ToGLM(leftPose.position) + ToGLM(rightPose.position)) * 0.5f;
                glm::quat ori = glm::slerp(ToGLM(leftPose.orientation), ToGLM(rightPose.orientation), 0.5f);
                glm::fmat4 headsetMatrix = ToMat4(mid, ori);
                bool inMenu = VRManager::instance().Hooks ? VRManager::instance().Hooks->IsShowingMenu() : false;
                auto actionsStart = std::chrono::high_resolution_clock::now();
                VRManager::instance().XR->UpdateActions(currentFrameState.predictedDisplayTime, headsetMatrix, inMenu);
                auto actionsStop = std::chrono::high_resolution_clock::now();
                s_linux->stats.updateActions.Add(std::chrono::duration<double, std::milli>(actionsStop - actionsStart).count());
            }
        }
        s_linux->submit.pendingCv.notify_all();
        s_linux->stats.waitBeginUpdate.Add(std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - totalStart).count());
    };

    runWaitBeginUpdate(); // bootstrap

    while (true) {
        if (s_linux->submit.stopRequested.load()) break;

        auto submitLoopStart = std::chrono::high_resolution_clock::now();
        if (s_linux->stats.lastSubmitLoopTime.time_since_epoch().count() != 0) {
            s_linux->stats.submitInterval.Add(std::chrono::duration<double, std::milli>(
                submitLoopStart - s_linux->stats.lastSubmitLoopTime).count());
        }
        s_linux->stats.lastSubmitLoopTime = submitLoopStart;
        s_linux->stats.submitFrames.fetch_add(1, std::memory_order_relaxed);

        // Drain OpenXR events on this thread too — if Cemu's thread is busy
        // (loading, etc.) it might not call ProcessEvents fast enough, and
        // unread events could back up. Try-lock so we never block Cemu's poll.
        VRManager::instance().XR->ProcessEvents();

        s_linux->submit.currentStep.store(1); // snapshot
        // Snapshot the latest content state Cemu has prepared
        LinuxRenderState::SubmitInfrastructure::LatestContent latest;
        {
            std::lock_guard<std::mutex> lock(s_linux->submit.latestMutex);
            latest = s_linux->submit.latest;
        }

        bool gateOk = latest.intermediateReadyValue > 0;
        bool has3D = gateOk && latest.has3D;
        bool hasHud = gateOk && latest.hasHud;
        bool hasDepth = latest.hasDepth && !s_linux->disableDepthReprojection;
        bool hasAnyLayerContent = has3D || hasHud;
        bool copyNeeded = hasAnyLayerContent
            && latest.contentSeq != 0
            && latest.contentSeq != lastCopiedContentSeq;
        bool reuseContent = hasAnyLayerContent
            && latest.contentSeq != 0
            && latest.contentSeq == lastCopiedContentSeq;

        if (copyNeeded) {
            s_linux->stats.submitFramesWithCopy.fetch_add(1, std::memory_order_relaxed);
            s_linux->stats.submitFramesWithNewContent.fetch_add(1, std::memory_order_relaxed);
        } else if (reuseContent) {
            s_linux->stats.submitFramesReusingContent.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_linux->stats.submitFramesNoContent.fetch_add(1, std::memory_order_relaxed);
        }

        XrCompositionLayerProjection projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        std::array<XrCompositionLayerProjectionView, 2> projectionViews = {};
        std::array<XrCompositionLayerDepthInfoKHR, 2> depthInfos = {};
        XrCompositionLayerQuad hudLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool projectionLayerReady = false;
        bool hudLayerReady = false;

        if (copyNeeded) {
            s_linux->submit.currentStep.store(2); // reset copy fence
            // The previous iteration waits for copyFence before releasing XR
            // swapchain images, so the top-of-loop wait was redundant.
            dispatch->ResetFences(device, 1, &s_linux->copyFence);

            s_linux->submit.currentStep.store(3); // acquire swapchains
            // Acquire swapchain images.
            auto acquireStart = std::chrono::high_resolution_clock::now();
            if (has3D) {
                for (int eye = 0; eye < 2; eye++) {
                    s_linux->colorSwapchains[eye].Acquire();
                    if (hasDepth) s_linux->depthSwapchains[eye].Acquire();
                }
            }
            if (hasHud && !s_linux->hudSwapchain.acquired) {
                s_linux->hudSwapchain.Acquire();
            }
            auto acquireStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.acquireSwapchains.Add(std::chrono::duration<double, std::milli>(
                acquireStop - acquireStart).count());

            // Record all copies (color/depth/hud) into one command buffer.
            auto recordStart = std::chrono::high_resolution_clock::now();
            VkCommandBufferBeginInfo cbBegin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            dispatch->BeginCommandBuffer(s_linux->copyCmdBuf, &cbBegin);

            auto recordColorCopy = [&](int eye) {
                if (!latest.eyeCopied[eye]) return;
                int readIdx = latest.readBufferIdx_3D[eye];
                if (readIdx < 0 || readIdx >= 2) return;
                auto* srcTex = s_linux->intermediateColor[eye][readIdx];
                if (!srcTex) return;
                VkImage srcImg = srcTex->GetImage();
                auto& sc = s_linux->colorSwapchains[eye];
                VkImage dstImg = sc.images[sc.currentImageIdx];
                VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                bar.srcAccessMask = 0;
                bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.image = dstImg;
                bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);
                VkImageCopy region = {};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.extent = { sc.width, sc.height, 1 };
                dispatch->CmdCopyImage(s_linux->copyCmdBuf,
                    srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);
            };

            auto recordDepthCopy = [&](int eye) {
                if (!latest.depthCopied[eye]) return;
                int readIdx = latest.readBufferIdx_3D[eye];
                if (readIdx < 0 || readIdx >= 2) return;
                auto* srcTex = s_linux->intermediateDepth[eye][readIdx];
                if (!srcTex) return;
                VkImage srcImg = srcTex->GetImage();
                auto& sc = s_linux->depthSwapchains[eye];
                VkImage dstImg = sc.images[sc.currentImageIdx];
                VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                bar.srcAccessMask = 0;
                bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.image = dstImg;
                bar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);
                VkImageCopy region = {};
                region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                region.extent = { sc.width, sc.height, 1 };
                dispatch->CmdCopyImage(s_linux->copyCmdBuf,
                    srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);
            };

            if (has3D) {
                for (int eye = 0; eye < 2; eye++) {
                    recordColorCopy(eye);
                    if (hasDepth) recordDepthCopy(eye);
                }
            }
            if (hasHud && s_linux->intermediateHud[latest.readBufferIdx_hud]) {
                VkImage hudSrc = s_linux->intermediateHud[latest.readBufferIdx_hud]->GetImage();
                auto& hudSc = s_linux->hudSwapchain;
                VkImage hudDst = hudSc.images[hudSc.currentImageIdx];
                VkImageMemoryBarrier hb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                hb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                hb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                hb.image = hudDst;
                hb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hb);
                VkImageCopy hc = {};
                hc.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                hc.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                hc.extent = { hudSc.width, hudSc.height, 1 };
                dispatch->CmdCopyImage(s_linux->copyCmdBuf, hudSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hudDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hc);
                hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                hb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                hb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                dispatch->CmdPipelineBarrier(s_linux->copyCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &hb);
            }

            s_linux->submit.currentStep.store(4); // end cmdbuf
            dispatch->EndCommandBuffer(s_linux->copyCmdBuf);
            auto recordStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.recordCopies.Add(std::chrono::duration<double, std::milli>(
                recordStop - recordStart).count());

            s_linux->submit.currentStep.store(5); // queue submit
            // Submit with cross-queue timeline semaphore wait. Cemu's queue 0
            // signaled intermediateReadyValue after its intermediate writes.
            // Queue 1 here waits for that signal before reading the intermediates.
            VkTimelineSemaphoreSubmitInfo waitTimeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            waitTimeline.waitSemaphoreValueCount = 1;
            waitTimeline.pWaitSemaphoreValues = &latest.intermediateReadyValue;
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo subInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            subInfo.pNext = &waitTimeline;
            subInfo.waitSemaphoreCount = 1;
            subInfo.pWaitSemaphores = &s_linux->submit.intermediateReady;
            subInfo.pWaitDstStageMask = &waitStage;
            subInfo.commandBufferCount = 1;
            subInfo.pCommandBuffers = &s_linux->copyCmdBuf;
            auto queueSubmitStart = std::chrono::high_resolution_clock::now();
            VkResult sr = dispatch->QueueSubmit(submitQueue, 1, &subInfo, s_linux->copyFence);
            auto queueSubmitStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.queueSubmitCopy.Add(std::chrono::duration<double, std::milli>(
                queueSubmitStop - queueSubmitStart).count());
            if (sr == VK_ERROR_DEVICE_LOST) {
                Log::print<ERROR>("Submit thread: QueueSubmit returned VK_ERROR_DEVICE_LOST — exiting (device unrecoverable)");
                break;
            }
            if (sr != VK_SUCCESS) {
                static int s_qsCount = 0;
                if (s_qsCount++ < 5) Log::print<ERROR>("Submit thread: QueueSubmit failed: {}", (int)sr);
            }
            s_linux->submit.currentStep.store(6); // wait copy fence
            // 5s timeout — generous enough that compositor stalls (motion
            // smoothing transitions, etc.) don't trip it, but bounded so we
            // can detect a genuine GPU hang.
            auto waitFenceStart = std::chrono::high_resolution_clock::now();
            VkResult fr = dispatch->WaitForFences(device, 1, &s_linux->copyFence, VK_TRUE, 5'000'000'000ULL /* 5s */);
            auto waitFenceStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.waitCopyFence.Add(std::chrono::duration<double, std::milli>(
                waitFenceStop - waitFenceStart).count());
            if (fr != VK_SUCCESS) {
                uint64_t semValue = 0;
                dispatch->GetSemaphoreCounterValue(device, s_linux->submit.intermediateReady, &semValue);
                Log::print<ERROR>("Submit thread: WaitForFences failed (result={}, sem wait={}, sem at={}) — exiting (fence reset on in-flight work would be UB)",
                    (int)fr, latest.intermediateReadyValue, semValue);
                break;
            }
            s_linux->submit.currentStep.store(7); // release swapchains

            auto releaseStart = std::chrono::high_resolution_clock::now();
            if (has3D) {
                for (int eye = 0; eye < 2; eye++) {
                    s_linux->colorSwapchains[eye].Release();
                    if (hasDepth) s_linux->depthSwapchains[eye].Release();
                }
            }
            if (hasHud) s_linux->hudSwapchain.Release();
            auto releaseStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.releaseSwapchains.Add(std::chrono::duration<double, std::milli>(
                releaseStop - releaseStart).count());
            lastCopiedContentSeq = latest.contentSeq;
        }

        if (hasAnyLayerContent) {
            // Late-latch the freshest pose for the projection + HUD layers.
            auto composeStart = std::chrono::high_resolution_clock::now();
            std::array<XrView, 2> submitViews = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
            std::chrono::high_resolution_clock::time_point sampleTime;
            XrTime sampleTargetTime = 0;
            bool haveSubmitViews = false;

            if (currentFrameState.predictedDisplayTime != 0 && stageSpace != XR_NULL_HANDLE) {
                XrViewLocateInfo lateLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
                lateLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                lateLocateInfo.displayTime = currentFrameState.predictedDisplayTime;
                lateLocateInfo.space = stageSpace;
                XrViewState lateViewState = { XR_TYPE_VIEW_STATE };
                uint32_t lateViewCount = 2;
                auto lateLocateStart = std::chrono::high_resolution_clock::now();
                XrResult lateResult = xrLocateViews(session, &lateLocateInfo, &lateViewState,
                    lateViewCount, &lateViewCount, submitViews.data());
                auto lateLocateStop = std::chrono::high_resolution_clock::now();
                s_linux->stats.lateLocateViews.Add(std::chrono::duration<double, std::milli>(
                    lateLocateStop - lateLocateStart).count());

                if (XR_SUCCEEDED(lateResult) && lateViewCount == 2 &&
                    (lateViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                    (lateViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                    sampleTime = lateLocateStop;
                    sampleTargetTime = currentFrameState.predictedDisplayTime;
                    haveSubmitViews = true;
                } else {
                    s_linux->stats.latePoseLocateFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (!haveSubmitViews) {
                std::lock_guard<std::mutex> lock(s_linux->submit.pendingMutex);
                if (s_linux->submit.pendingViews.has_value()) {
                    submitViews = *s_linux->submit.pendingViews;
                    sampleTargetTime = s_linux->submit.pendingFrameState.predictedDisplayTime;
                    haveSubmitViews = true;
                }
            }
            if (!haveSubmitViews) {
                std::lock_guard<std::mutex> lock(s_linux->poseTracking.viewsMutex);
                if (s_linux->poseTracking.latestRawViews.has_value()) {
                    submitViews = *s_linux->poseTracking.latestRawViews;
                    sampleTime = s_linux->poseTracking.latestSampleWallTime;
                    sampleTargetTime = s_linux->poseTracking.latestSampleTargetTime;
                    haveSubmitViews = true;
                }
            }

            // Record pose-age stat for the fresh pose used by HUD composition
            // and as a fallback when a 3D content pose is unavailable.
            if (haveSubmitViews && sampleTime.time_since_epoch().count() != 0) {
                double ageMs = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - sampleTime).count();
                s_linux->stats.poseAge.Add(ageMs);
            }
            if (haveSubmitViews && sampleTargetTime != 0 && currentFrameState.predictedDisplayTime != 0) {
                s_linux->stats.poseTargetDelta.Add((double)(sampleTargetTime - currentFrameState.predictedDisplayTime) / 1e6);
            }
            if (latest.publishedAt.time_since_epoch().count() != 0) {
                s_linux->stats.contentAge.Add(std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - latest.publishedAt).count());
            }
            if (latest.hasRenderViews && latest.renderDisplayTime != 0 && currentFrameState.predictedDisplayTime != 0) {
                s_linux->stats.contentPoseDelta.Add(
                    (double)(latest.renderDisplayTime - currentFrameState.predictedDisplayTime) / 1e6);
            }

            if (haveSubmitViews && has3D) {
                const std::array<XrView, 2>* projectionRenderViews = &submitViews;
                if (latest.hasRenderViews) {
                    projectionRenderViews = &latest.renderViews;
                    s_linux->stats.projectionFramesUsingContentPose.fetch_add(1, std::memory_order_relaxed);
                } else {
                    s_linux->stats.projectionFramesFreshPoseFallback.fetch_add(1, std::memory_order_relaxed);
                    static int s_missingRenderViewsWarnings = 0;
                    if (s_missingRenderViewsWarnings++ < 5) {
                        Log::print<WARNING>("Submit thread: 3D content has no render-pose snapshot; using fresh pose fallback");
                    }
                }
                for (int eye = 0; eye < 2; eye++) {
                    if (hasDepth) {
                        auto& di = depthInfos[eye];
                        di.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                        di.next = nullptr;
                        di.subImage.swapchain = s_linux->depthSwapchains[eye].handle;
                        di.subImage.imageRect.offset = { 0, 0 };
                        di.subImage.imageRect.extent = {
                            (int32_t)s_linux->depthSwapchains[eye].width,
                            (int32_t)s_linux->depthSwapchains[eye].height
                        };
                        di.subImage.imageArrayIndex = 0;
                        di.minDepth = 0.0f;
                        di.maxDepth = 1.0f;
                        di.nearZ = GetSettings().GetZNear();
                        di.farZ = GetSettings().GetZFar();
                    }
                    auto& pv = projectionViews[eye];
                    pv.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    pv.next = hasDepth ? &depthInfos[eye] : nullptr;
                    pv.pose = (*projectionRenderViews)[eye].pose;
                    pv.fov = (*projectionRenderViews)[eye].fov;
                    pv.subImage.swapchain = s_linux->colorSwapchains[eye].handle;
                    pv.subImage.imageRect.offset = { 0, 0 };
                    pv.subImage.imageRect.extent = {
                        (int32_t)s_linux->colorSwapchains[eye].width,
                        (int32_t)s_linux->colorSwapchains[eye].height
                    };
                    pv.subImage.imageArrayIndex = 0;

                    static int s_projectionSubmitLogs[2] = {};
                    if (s_linux->captureDiagnosticsEnabled && s_projectionSubmitLogs[eye]++ < 6) {
                        const XrView& rv = (*projectionRenderViews)[eye];
                        const double renderDeltaMs =
                            (latest.renderDisplayTime != 0 && currentFrameState.predictedDisplayTime != 0)
                                ? (double)(latest.renderDisplayTime - currentFrameState.predictedDisplayTime) / 1e6
                                : 0.0;
                        Log::print<INFO>(
                            "Submit projection eye={} poseSource={} depth={} renderDelta={:.2f}ms pos=({:.3f},{:.3f},{:.3f}) fov=({:.1f},{:.1f},{:.1f},{:.1f})",
                            eye,
                            latest.hasRenderViews ? "content" : "fresh",
                            hasDepth ? "yes" : "no",
                            renderDeltaMs,
                            rv.pose.position.x, rv.pose.position.y, rv.pose.position.z,
                            glm::degrees(rv.fov.angleLeft), glm::degrees(rv.fov.angleRight),
                            glm::degrees(rv.fov.angleUp), glm::degrees(rv.fov.angleDown));
                    }
                }
                projectionLayer.space = stageSpace;
                projectionLayer.viewCount = 2;
                projectionLayer.views = projectionViews.data();
                projectionLayerReady = true;
            }

            if (haveSubmitViews && hasHud) {
                glm::vec3 headPos = (ToGLM(submitViews[0].pose.position) + ToGLM(submitViews[1].pose.position)) * 0.5f;
                glm::quat headOri = glm::slerp(ToGLM(submitViews[0].pose.orientation), ToGLM(submitViews[1].pose.orientation), 0.5f);
                constexpr float HUD_DISTANCE = 2.0f;
                constexpr float HUD_SIZE = 1.0f;
                // Keep the HUD hard-locked to the latest submit pose. Smoothing
                // this orientation made the quad lag head rotation and produced
                // a single-frame jello/shear artifact in both HUD and 3D content.
                glm::vec3 forward = headOri * glm::vec3(0.0f, 0.0f, -1.0f);
                glm::vec3 hudPos = headPos + (HUD_DISTANCE * forward);
                glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
                glm::vec3 right = glm::cross(forward, worldUp);
                if (glm::length2(right) < 1.0e-6f) {
                    worldUp = glm::vec3(0.0f, 0.0f, 1.0f);
                    right = glm::cross(forward, worldUp);
                }
                right = glm::normalize(right);
                glm::vec3 up = glm::cross(right, forward);
                glm::quat hudOri = glm::quatLookAt(forward, up);
                constexpr float aspectRatio = 16.0f / 9.0f;
                hudLayer.layerFlags = has3D ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                hudLayer.space = stageSpace;
                hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudLayer.subImage.swapchain = s_linux->hudSwapchain.handle;
                hudLayer.subImage.imageRect.offset = { 0, 0 };
                hudLayer.subImage.imageRect.extent = {
                    (int32_t)s_linux->hudSwapchain.width,
                    (int32_t)s_linux->hudSwapchain.height
                };
                hudLayer.pose.orientation = ToXR(hudOri);
                hudLayer.pose.position = ToXR(hudPos);
                hudLayer.size = { aspectRatio * HUD_SIZE, 1.0f * HUD_SIZE };
                hudLayerReady = true;
            }
            auto composeStop = std::chrono::high_resolution_clock::now();
            s_linux->stats.composeLayers.Add(std::chrono::duration<double, std::milli>(
                composeStop - composeStart).count());
        }

        std::vector<const XrCompositionLayerBaseHeader*> layers;
        if (projectionLayerReady) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer));
        if (hudLayerReady) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer));
        if (!layers.empty()) {
            s_linux->stats.submitFramesWithLayers.fetch_add(1, std::memory_order_relaxed);
        }

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = currentFrameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (uint32_t)layers.size();
        endInfo.layers = layers.empty() ? nullptr : layers.data();

        s_linux->submit.currentStep.store(8); // xrEndFrame

        // Per OpenXR spec, xrEndFrame is only meaningful in VISIBLE or FOCUSED
        // states. In other states (SYNCHRONIZED, STOPPING, etc.) the runtime
        // may discard frames or behave unpredictably. We still call it (because
        // the spec requires a matched End for each Begin), but log when we're
        // in a state where this could be the cause of issues.
        XrSessionState sessState = VRManager::instance().XR->m_currentSessionState.load(std::memory_order_acquire);
        bool stateOkForRender = (sessState == XR_SESSION_STATE_VISIBLE
                              || sessState == XR_SESSION_STATE_FOCUSED);

        // Optional heartbeat for timing/debug runs.
        uint64_t curIter = s_linux->submit.iterCount.load(std::memory_order_relaxed);
        if (s_linux->frameTimingStatsEnabled && curIter > 0 && curIter % 600 == 0) {
            Log::print<INFO>("Submit thread heartbeat: iter={}, sessionState={}, stateOk={}",
                curIter, (int)sessState, stateOkForRender);
        }

        auto xrEndStart = std::chrono::high_resolution_clock::now();
        XrResult xrEndResult = xrEndFrame(session, &endInfo);
        auto xrEndStop = std::chrono::high_resolution_clock::now();
        double xrEndMs = std::chrono::duration<double, std::milli>(xrEndStop - xrEndStart).count();
        s_linux->stats.xrEndFrame.Add(xrEndMs);
        if (xrEndMs > 100.0) {
            Log::print<WARNING>("xrEndFrame took {:.1f}ms (result: {}, sessionState: {})",
                xrEndMs, (int)xrEndResult, (int)sessState);
        }
        if (XR_FAILED(xrEndResult)) {
            static int s_errCount = 0;
            if (s_errCount++ < 5) {
                Log::print<ERROR>("xrEndFrame returned error: {} — session may be lost", (int)xrEndResult);
            }
            // If session is lost or unrecoverable error, stop the loop.
            if (xrEndResult == XR_ERROR_SESSION_LOST || xrEndResult == XR_ERROR_RUNTIME_FAILURE
                || xrEndResult == XR_ERROR_INSTANCE_LOST) {
                Log::print<ERROR>("Submit thread: exiting due to fatal xrEndFrame error");
                break;
            }
        }

        s_linux->submit.currentStep.store(9); // runWaitBeginUpdate
        // Move to next vsync slot (xrWaitFrame blocks, this is our pacing).
        runWaitBeginUpdate();
        s_linux->submit.iterCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (s_linux) s_linux->submit.alive.store(false, std::memory_order_release);
    Log::print<INFO>("Submit thread: stopped");
}

void RND_Renderer::StartFrame() {
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    // Frame interval = wall clock between consecutive StartFrame calls.
    if (s_linux && s_linux->stats.lastPresentTime.time_since_epoch().count() != 0) {
        double intervalMs = std::chrono::duration<double, std::milli>(
            m_frameStartTime - s_linux->stats.lastPresentTime).count();
        s_linux->stats.frameInterval.Add(intervalMs);
    }
    if (s_linux) s_linux->stats.lastPresentTime = m_frameStartTime;

    // Phase 5+6: StartFrame is no longer responsible for xrWaitFrame/UpdateViews.
    // The submit thread runs the full OpenXR lifecycle after xrEndFrame and
    // stores fresh poses in pendingViews. We mark this Cemu frame as
    // "needs latch" and then perform the latch right away so m_currViews is
    // populated for code paths that read it without going through a hook
    // (the unified-copy hasHud/has3DContent checks, HUD gaze positioning, etc.).
    // In steady state this latch is near-instant because submit thread has
    // ~21ms of Cemu's previous render to prepare pendingViews.
    if (s_linux) {
        m_linuxNeedsLatch.store(true, std::memory_order_release);

        // Launch the pose-sampling thread + submit thread on first frame.
        // The submit thread's bootstrap handles the initial xrWaitFrame.
        if (!s_linux->submit.thread.joinable() && !s_linux->submit.stopRequested.load()) {
            XrSpace stageSpace = VRManager::instance().XR->m_stageSpace;
            s_linux->submit.thread = std::thread(SubmitThreadMain, m_session, stageSpace);
        }
        if (!s_linux->poseTracking.running.load(std::memory_order_acquire)) {
            XrSpace stageSpace = VRManager::instance().XR->m_stageSpace;
            // Pose thread needs a timing reference. The submit thread refreshes
            // it immediately after each xrWaitFrame, including the bootstrap call.
            XrTime baseTime = s_linux->poseTracking.baseDisplayTime.load(std::memory_order_acquire);
            if (baseTime != 0 && stageSpace != XR_NULL_HANDLE) {
                s_linux->poseTracking.running.store(true, std::memory_order_release);
                s_linux->poseTracking.thread = std::thread(PoseTrackingThreadMain, m_session, stageSpace);
            }
        }
    }

    // EndFrame reads pendingFrameState directly under lock; no need to mirror.

    // Eagerly latch pendingViews → m_currViews so EndFrame's hasHud / has3DContent
    // checks see valid views even when no camera hook reads poses (e.g., title
    // screen where Cemu doesn't run VR camera code). LatchPendingIfNeeded is
    // idempotent; if a camera hook later fires it'll see needsLatch=false and
    // proceed with the already-latched m_currViews.
    LatchPendingIfNeeded();

    // Reset per-frame state
    if (s_linux) {
        s_linux->hudCopied = false;
    }

    if (s_linux) {
        auto startFrameEnd = std::chrono::high_resolution_clock::now();
        s_linux->stats.startFrame.Add(std::chrono::duration<double, std::milli>(startFrameEnd - m_frameStartTime).count());
        s_linux->stats.startFrameExitTime = startFrameEnd;
    }
}

void RND_Renderer::EndFrame() {
    auto endFrameEntry = std::chrono::high_resolution_clock::now();
    if (s_linux) {
        s_linux->stats.endFrameEntryTime = endFrameEntry;
        if (s_linux->stats.startFrameExitTime.time_since_epoch().count() != 0) {
            s_linux->stats.cemuWork.Add(std::chrono::duration<double, std::milli>(
                endFrameEntry - s_linux->stats.startFrameExitTime).count());
        }
    }

    if (m_cameraIsCapturing3DFrameBuffer > 0) {
        --m_cameraIsCapturing3DFrameBuffer;
    }

    if (!s_linux) return;

    // Phase 7: just signal the timeline semaphore and update the latest content
    // state. The submit thread does all GPU copies + xrEndFrame at native
    // vsync rate, re-presenting copied content between Cemu pushes.
    float fadeAmount = m_customFadeAmount.load(std::memory_order_relaxed);
    bool fadeActive = fadeAmount > 0.5f || m_isFadeActive.load(std::memory_order_relaxed);
    std::optional<std::array<XrView, 2>> currentViews;
    XrTime currentViewsDisplayTime = 0;
    {
        std::shared_lock viewsLock(m_viewsMutex);
        currentViews = m_currViews;
        currentViewsDisplayTime = m_linuxCurrViewsDisplayTime;
    }
    bool hasViews = currentViews.has_value();
    const auto& published3DPair = s_linux->completed3DPair;
    bool hasCompleted3DPair = published3DPair.IsComplete();
    auto pairDepthCopied = [&](int eye) {
        if (eye < 0 || eye >= 2) return false;
        int bufferIdx = published3DPair.bufferIdx[eye];
        return bufferIdx >= 0 && bufferIdx < 2 && s_linux->captured3DFrames[bufferIdx].depthCopied[eye];
    };
    bool hasDepth = !s_linux->disableDepthReprojection
        && hasCompleted3DPair
        && pairDepthCopied(0)
        && pairDepthCopied(1);
    bool has3DContent = s_linux->frameHasContent && hasCompleted3DPair && hasViews && !fadeActive;
    bool hasHud = s_linux->hudCopied && s_linux->hudSwapchain.handle != XR_NULL_HANDLE && hasViews;
    if (has3DContent &&
        published3DPair.colorCopyAt[0].time_since_epoch().count() != 0 &&
        published3DPair.colorCopyAt[1].time_since_epoch().count() != 0) {
        s_linux->stats.eyeCaptureDelta.Add(std::chrono::duration<double, std::milli>(
            published3DPair.colorCopyAt[1] - published3DPair.colorCopyAt[0]).count());
    }

    if (has3DContent || hasHud) {
        auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
        auto device = VRManager::instance().VK->GetDevice();
        VkQueue queue0 = VK_NULL_HANDLE;
        dispatch->GetDeviceQueue(device, VRManager::instance().VK->GetQueueFamilyIndex(), 0, &queue0);

        // Queue ordering guarantees this signal happens AFTER Cemu's already-
        // submitted intermediate writes (our LinuxCopyXxxToSwapchain hooks are
        // baked into Cemu's command buffer earlier on the same queue).
        uint64_t signalValue = s_linux->submit.nextSignalValue.fetch_add(1, std::memory_order_acq_rel);

        // Update intermediate-ready and currently-active flags. We don't blank
        // out has3D/hasHud when this Cemu frame had no captures — leaving them
        // true lets the submit thread keep re-presenting the previous valid
        // intermediates, avoiding black flashes on frames where Cemu didn't
        // push new content (cutscene transitions, etc.).
        VkTimelineSemaphoreSubmitInfo timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &signalValue;
        VkSubmitInfo signalSubmit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        signalSubmit.pNext = &timelineInfo;
        signalSubmit.signalSemaphoreCount = 1;
        signalSubmit.pSignalSemaphores = &s_linux->submit.intermediateReady;
        auto signalStart = std::chrono::high_resolution_clock::now();
        VkResult signalRes = dispatch->QueueSubmit(queue0, 1, &signalSubmit, VK_NULL_HANDLE);
        auto signalStop = std::chrono::high_resolution_clock::now();
        s_linux->stats.cemuSignal.Add(std::chrono::duration<double, std::milli>(
            signalStop - signalStart).count());

        if (signalRes == VK_SUCCESS) {
            // Publish per-layer read indices ONLY for layers Cemu actually wrote
            // this frame. Sticky flags keep has3D/hasHud true once seen, but each
            // layer's read buffer index only updates when that layer was written,
            // otherwise submit thread would read an UNDEFINED buffer.
            int publishedHud = s_linux->currentWriteIdx_hud;
            auto publishedAt = std::chrono::high_resolution_clock::now();
            {
                std::lock_guard<std::mutex> lock(s_linux->submit.latestMutex);
                s_linux->submit.latest.intermediateReadyValue = signalValue;
                s_linux->submit.latest.contentSeq++;
                s_linux->submit.latest.publishedAt = publishedAt;
                if (has3DContent) {
                    s_linux->submit.latest.has3D = true;
                    s_linux->submit.latest.hasDepth = hasDepth;
                    s_linux->submit.latest.readBufferIdx_3D[0] = published3DPair.bufferIdx[0];
                    s_linux->submit.latest.readBufferIdx_3D[1] = published3DPair.bufferIdx[1];
                    s_linux->submit.latest.renderViews = published3DPair.hasRenderViews
                        ? published3DPair.renderViews
                        : *currentViews;
                    s_linux->submit.latest.renderDisplayTime = published3DPair.hasRenderViews
                        ? published3DPair.renderDisplayTime
                        : currentViewsDisplayTime;
                    s_linux->submit.latest.hasRenderViews = true;
                    s_linux->submit.latest.eyeCopied[0] = published3DPair.eyeCopied[0];
                    s_linux->submit.latest.eyeCopied[1] = published3DPair.eyeCopied[1];
                    s_linux->submit.latest.depthCopied[0] = pairDepthCopied(0);
                    s_linux->submit.latest.depthCopied[1] = pairDepthCopied(1);
                }
                if (hasHud) {
                    s_linux->submit.latest.hasHud = true;
                    s_linux->submit.latest.hudCopied = true;
                    s_linux->submit.latest.readBufferIdx_hud = publishedHud;
                }
                s_linux->submit.latest.customFadeAmount = fadeAmount;
                s_linux->submit.latest.fadeActive = fadeActive;
            }
            s_linux->stats.cemuPublishes.fetch_add(1, std::memory_order_relaxed);
            if (has3DContent) s_linux->stats.cemuPublishes3D.fetch_add(1, std::memory_order_relaxed);
            if (hasHud) s_linux->stats.cemuPublishesHud.fetch_add(1, std::memory_order_relaxed);
            // 3D buffers are selected by BotW's encoded magic frame index.
            if (has3DContent)  s_linux->frameHasContent = false;
            if (hasHud)        s_linux->currentWriteIdx_hud ^= 1;
        }
        else {
            s_linux->stats.cemuSignalFailures.fetch_add(1, std::memory_order_relaxed);
            Log::print<ERROR>("EndFrame: signal-only QueueSubmit failed: {}", (int)signalRes);
        }
    }

    auto frameEnd = std::chrono::high_resolution_clock::now();
    m_lastFrameWorkTimeMs.store(std::chrono::duration<double, std::milli>(frameEnd - m_frameStartTime).count(),
                                std::memory_order_relaxed);
    s_linux->stats.endFrame.Add(std::chrono::duration<double, std::milli>(frameEnd - s_linux->stats.endFrameEntryTime).count());

    if (s_linux->frameTimingStatsEnabled
        && ++s_linux->stats.framesLogged >= LinuxRenderState::FrameStats::LOG_EVERY_N_FRAMES) {
        s_linux->stats.framesLogged = 0;
        auto logNow = std::chrono::steady_clock::now();
        auto sFI = s_linux->stats.frameInterval.Compute();
        auto sWF = s_linux->stats.waitFrame.Compute();
        auto sBF = s_linux->stats.beginFrame.Compute();
        auto sLV = s_linux->stats.locateViews.Compute();
        auto sLL = s_linux->stats.lateLocateViews.Compute();
        auto sUA = s_linux->stats.updateActions.Compute();
        auto sWBU = s_linux->stats.waitBeginUpdate.Compute();
        auto sSF = s_linux->stats.startFrame.Compute();
        auto sCW = s_linux->stats.cemuWork.Compute();
        auto sEF = s_linux->stats.endFrame.Compute();
        auto sCS = s_linux->stats.cemuSignal.Compute();
        auto sSI = s_linux->stats.submitInterval.Compute();
        auto sACQ = s_linux->stats.acquireSwapchains.Compute();
        auto sRC = s_linux->stats.recordCopies.Compute();
        auto sQSC = s_linux->stats.queueSubmitCopy.Compute();
        auto sWCF = s_linux->stats.waitCopyFence.Compute();
        auto sREL = s_linux->stats.releaseSwapchains.Compute();
        auto sCL = s_linux->stats.composeLayers.Compute();
        auto sXE = s_linux->stats.xrEndFrame.Compute();
        auto sPA = s_linux->stats.poseAge.Compute();
        auto sPTD = s_linux->stats.poseTargetDelta.Compute();
        auto sCA = s_linux->stats.contentAge.Compute();
        auto sCPD = s_linux->stats.contentPoseDelta.Compute();
        auto sECD = s_linux->stats.eyeCaptureDelta.Compute();
        double elapsedSec = 0.0;
        if (s_linux->stats.lastStatsLogTime.time_since_epoch().count() != 0) {
            elapsedSec = std::chrono::duration<double>(logNow - s_linux->stats.lastStatsLogTime).count();
        }
        if (elapsedSec <= 0.001) {
            elapsedSec = sFI.mean > 0.0
                ? (sFI.mean * (double)LinuxRenderState::FrameStats::LOG_EVERY_N_FRAMES) / 1000.0
                : 2.0;
        }
        s_linux->stats.lastStatsLogTime = logNow;

        uint64_t poseSamples = s_linux->poseTracking.sampleCount.exchange(0, std::memory_order_relaxed);
        uint64_t poseFails = s_linux->poseTracking.failureCount.exchange(0, std::memory_order_relaxed);
        uint64_t submitFrames = s_linux->stats.submitFrames.exchange(0, std::memory_order_relaxed);
        uint64_t submitLayers = s_linux->stats.submitFramesWithLayers.exchange(0, std::memory_order_relaxed);
        uint64_t submitCopies = s_linux->stats.submitFramesWithCopy.exchange(0, std::memory_order_relaxed);
        uint64_t submitNew = s_linux->stats.submitFramesWithNewContent.exchange(0, std::memory_order_relaxed);
        uint64_t submitReused = s_linux->stats.submitFramesReusingContent.exchange(0, std::memory_order_relaxed);
        uint64_t submitEmpty = s_linux->stats.submitFramesNoContent.exchange(0, std::memory_order_relaxed);
        uint64_t cemuPublishes = s_linux->stats.cemuPublishes.exchange(0, std::memory_order_relaxed);
        uint64_t cemuPublishes3D = s_linux->stats.cemuPublishes3D.exchange(0, std::memory_order_relaxed);
        uint64_t cemuPublishesHud = s_linux->stats.cemuPublishesHud.exchange(0, std::memory_order_relaxed);
        uint64_t signalFails = s_linux->stats.cemuSignalFailures.exchange(0, std::memory_order_relaxed);
        uint64_t latePoseFails = s_linux->stats.latePoseLocateFailures.exchange(0, std::memory_order_relaxed);
        uint64_t projectionContentPose = s_linux->stats.projectionFramesUsingContentPose.exchange(0, std::memory_order_relaxed);
        uint64_t projectionFreshFallback = s_linux->stats.projectionFramesFreshPoseFallback.exchange(0, std::memory_order_relaxed);
        double poseRate = poseSamples / elapsedSec;
        double submitHz = submitFrames / elapsedSec;
        double cemuHz = (double)LinuxRenderState::FrameStats::LOG_EVERY_N_FRAMES / elapsedSec;
        uint64_t contentSubmits = submitNew + submitReused;
        double reusePct = contentSubmits > 0 ? (100.0 * (double)submitReused / (double)contentSubmits) : 0.0;
        Log::print<INFO>(
            "[frame stats] cemu={:.1f}Hz interval p50={:.2f} p99={:.2f}ms | start p50={:.3f}ms | cemuWork p50={:.2f} p99={:.2f}ms | end p50={:.3f} p99={:.3f}ms | cemuSignal p50={:.3f} p99={:.3f}ms",
            cemuHz, sFI.p50, sFI.p99, sSF.p50, sCW.p50, sCW.p99, sEF.p50, sEF.p99, sCS.p50, sCS.p99);
        Log::print<INFO>(
            "[submit stats] submit={:.1f}Hz interval p50={:.2f} p99={:.2f}ms | xrWait p50={:.2f} p99={:.2f}ms | xrBegin p50={:.3f}ms | locate p50={:.3f} p99={:.3f}ms | lateLocate p50={:.3f} p99={:.3f}ms fails={} | actions p50={:.3f} p99={:.3f}ms | wait+update p50={:.2f} p99={:.2f}ms",
            submitHz, sSI.p50, sSI.p99, sWF.p50, sWF.p99, sBF.p50, sLV.p50, sLV.p99,
            sLL.p50, sLL.p99, latePoseFails, sUA.p50, sUA.p99, sWBU.p50, sWBU.p99);
        Log::print<INFO>(
            "[copy stats] acquire p50={:.3f} p99={:.3f}ms | record p50={:.3f} p99={:.3f}ms | qsubmit p50={:.3f} p99={:.3f}ms | fence p50={:.3f} p99={:.3f}ms | release p50={:.3f} p99={:.3f}ms | compose p50={:.3f} p99={:.3f}ms | xrEnd p50={:.2f} p99={:.2f}ms",
            sACQ.p50, sACQ.p99, sRC.p50, sRC.p99, sQSC.p50, sQSC.p99,
            sWCF.p50, sWCF.p99, sREL.p50, sREL.p99, sCL.p50, sCL.p99, sXE.p50, sXE.p99);
        Log::print<INFO>(
            "[pose/content stats] poseAge p50={:.2f} p99={:.2f}ms | poseTargetDelta p50={:.2f} p99={:.2f}ms min={:.2f} max={:.2f} | contentAge p50={:.2f} p99={:.2f}ms | contentPoseDelta p50={:.2f} p99={:.2f}ms min={:.2f} max={:.2f} | eyeCaptureDelta p50={:.2f} p99={:.2f}ms | poseThread {:.0f}Hz fails={} | submit frames={} layers={} copies={} new={} reused={} ({:.0f}% of content submits) empty={} | projPose content={} freshFallback={} | cemu publishes={} 3d={} hud={} signalFails={}",
            sPA.p50, sPA.p99, sPTD.p50, sPTD.p99, sPTD.min, sPTD.max,
            sCA.p50, sCA.p99, sCPD.p50, sCPD.p99, sCPD.min, sCPD.max,
            sECD.p50, sECD.p99, poseRate, poseFails, submitFrames, submitLayers, submitCopies,
            submitNew, submitReused, reusePct, submitEmpty, projectionContentPose, projectionFreshFallback,
            cemuPublishes, cemuPublishes3D, cemuPublishesHud, signalFails);

        if (s_linux->captureDiagnosticsEnabled) {
            uint64_t diagColorL = s_linux->stats.diag3DColorCopies[0].exchange(0, std::memory_order_relaxed);
            uint64_t diagColorR = s_linux->stats.diag3DColorCopies[1].exchange(0, std::memory_order_relaxed);
            uint64_t diagDepthL = s_linux->stats.diag3DDepthCopies[0].exchange(0, std::memory_order_relaxed);
            uint64_t diagDepthR = s_linux->stats.diag3DDepthCopies[1].exchange(0, std::memory_order_relaxed);
            uint64_t diagPendingLeft = s_linux->stats.diag3DPendingLeftStarts.exchange(0, std::memory_order_relaxed);
            uint64_t diagPairs = s_linux->stats.diag3DCompletedPairs.exchange(0, std::memory_order_relaxed);
            uint64_t diagOrphanRight = s_linux->stats.diag3DOrphanRightDrops.exchange(0, std::memory_order_relaxed);
            uint64_t pauseEpoch = s_linux->pauseDiag.epoch.load(std::memory_order_acquire);
            bool inInventoryPause = s_linux->pauseDiag.inInventoryPause.load(std::memory_order_acquire);
            auto statsPendingPair = s_linux->pending3DPair;
            auto statsCompletedPair = s_linux->completed3DPair;
            LinuxRenderState::SubmitInfrastructure::LatestContent statsLatest;
            {
                std::lock_guard<std::mutex> lock(s_linux->submit.latestMutex);
                statsLatest = s_linux->submit.latest;
            }
            Log::print<INFO>(
                "[capture state] pause epoch={} in={} capColor=({},{}) capDepth=({},{}) pairStart={} pairDone={} orphanR={} pending=({},{} idx={}, {}) completed=({},{} idx={}, {}) latest3D={} latestHud={} latestIdx=({}, {}) latestSeq={}",
                pauseEpoch, inInventoryPause ? "yes" : "no",
                diagColorL, diagColorR, diagDepthL, diagDepthR,
                diagPendingLeft, diagPairs, diagOrphanRight,
                statsPendingPair.eyeCopied[0] ? "yes" : "no",
                statsPendingPair.eyeCopied[1] ? "yes" : "no",
                statsPendingPair.bufferIdx[0], statsPendingPair.bufferIdx[1],
                statsCompletedPair.eyeCopied[0] ? "yes" : "no",
                statsCompletedPair.eyeCopied[1] ? "yes" : "no",
                statsCompletedPair.bufferIdx[0], statsCompletedPair.bufferIdx[1],
                statsLatest.has3D ? "yes" : "no",
                statsLatest.hasHud ? "yes" : "no",
                statsLatest.readBufferIdx_3D[0], statsLatest.readBufferIdx_3D[1],
                statsLatest.contentSeq);
        }
    }
}

// Compute the UV sub-region for this eye: maps from the expanded 16:9 render to the eye's actual FOV
static RenderUtils::UvTransform ComputeEyeUvTransform(OpenXR::EyeSide side, float renderAspectRatio) {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (!renderer) return {};

    auto rawFov = renderer->GetFOV(side);
    return RenderUtils::GetPresentationUvTransform(rawFov, renderAspectRatio);
}

// Linux capture dispatch for the CmdClearColorImage hook. Moved verbatim out of the
// shared framebuffer.cpp so that hook stays a thin dispatcher. Runs the 3D-color
// (captureIdx 0) and 2D-HUD (captureIdx 2) capture paths, then issues the transparent
// passthrough clear. srcWidth/srcHeight/srcFormat are the source image's tracked
// dimensions (looked up by the hook); hudCapturedThisFrame is the per-frame HUD-once
// flag the hook owns (also reset at QueuePresentKHR).
void RND_Renderer::LinuxHandleColorClear(OpenXR::EyeSide side, long captureIdx, long frameIdx,
        const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image,
        VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount,
        const VkImageSubresourceRange* pRanges, uint32_t srcWidth, uint32_t srcHeight, VkFormat srcFormat,
        bool& hudCapturedThisFrame) {
    const bool captureDiag = VRLayer::LinuxCaptureDiagnosticsEnabled();
    {
        static VkImage last3Dimg = VK_NULL_HANDLE;
        static VkImage last2Dimg = VK_NULL_HANDLE;
        static VkFormat last3Dfmt = VK_FORMAT_UNDEFINED;
        static VkFormat last2Dfmt = VK_FORMAT_UNDEFINED;
        if (srcWidth > 0) {
            if (captureIdx == 0) { last3Dimg = image; last3Dfmt = srcFormat; }
            if (captureIdx == 2) { last2Dimg = image; last2Dfmt = srcFormat; }
        }
        if (captureDiag && last3Dimg != VK_NULL_HANDLE && last2Dimg != VK_NULL_HANDLE) {
            static uint32_t gameplayLogCount = 0;
            if (++gameplayLogCount <= 5) {
                Log::print<INFO>("GAMEPLAY Capture: idx={} side={} img={} | 3D={} (fmt={}) 2D={} (fmt={}) same={}",
                    captureIdx, (int)side, (void*)image,
                    (void*)last3Dimg, (int)last3Dfmt, (void*)last2Dimg, (int)last2Dfmt,
                    last3Dimg == last2Dimg ? "YES" : "no");
            }
        }
    }
    if (captureDiag) {
        static bool seen3D = false;
        static int seqLog = 0;
        if (captureIdx == 0) seen3D = true;
        if (seen3D && ++seqLog <= 30) {
            // Log the raw R value to see ALL capture indices, not just 0 and 2.
            float rawR = pColor->float32[0];
            long rawIdx = std::lroundf(rawR * 32.0f);
            Log::print<INFO>("SEQ: rawIdx={} (R={:.4f}) side={} img={}", rawIdx, rawR, (int)side, (void*)image);
        }
    }

    if (srcWidth > 0 && srcHeight > 0) {
        // Transition source to GENERAL for reading
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL);

        if (captureIdx == 0) {
            const bool is3DColorTarget = srcFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            if (is3DColorTarget) {
                hudCapturedThisFrame = false;
                // Apply pause transitions only at a left-eye boundary. The menu state
                // often flips between the left and right clears; resetting there strands
                // the right eye as an orphan and perturbs the next gameplay pair.
                const int pauseTransition = side == OpenXR::EyeSide::LEFT
                    ? VRLayer::UpdateLinuxInventoryPauseTransition()
                    : 0;
                if (pauseTransition != 0) {
                    LinuxMarkInventoryPauseTransition(pauseTransition > 0);
                    if (captureDiag) {
                        VRLayer::LogLinuxInventoryPauseDetectorState(
                            pauseTransition > 0 ? "opened" : "closed",
                            this, side, frameIdx, srcFormat, srcWidth, srcHeight);
                    }
                }
                // Keep the normal startup capture path alive even while inventory screens
                // are visible; suppressing these copies can strand the submit thread in
                // HUD-only mode if the pause screen state lingers after close.
                LinuxCopyColorToSwapchain(side, commandBuffer, image, srcWidth, srcHeight, frameIdx);
            } else {
                static int s_skipped3DTargetLogs = 0;
                if (captureDiag && s_skipped3DTargetLogs++ < 6) {
                    Log::print<INFO>("Linux 3D capture skipped non-display target: side={} img={} format={}",
                        (int)side, (void*)image, (int)srcFormat);
                }
            }

            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VulkanUtils::DebugPipelineBarrier(commandBuffer);

            // Clear with alpha=0 (transparent) so HUD elements show with transparency
            VkClearColorValue clearColor = { { 0.0f, 0.0f, 0.0f, 0.0f } };
            pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, &clearColor, rangeCount, pRanges);
            return;
        } else if (captureIdx == 2) {
            // 2D HUD - capture first occurrence only per frame (captureIdx=2 fires on two
            // images; the second may overwrite valid HUD content).
            if (!hudCapturedThisFrame) {
                LinuxCopyHudToSwapchain(commandBuffer, image, srcWidth, srcHeight);
                hudCapturedThisFrame = true;
            }
            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VkClearColorValue clearColor = { { 0.0f, 0.0f, 0.0f, 0.0f } };
            pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, &clearColor, rangeCount, pRanges);
            return;
        }
    }
    // Pass through with magic values replaced by transparent black
    VkClearColorValue clearColor = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, &clearColor, rangeCount, pRanges);
}

// Linux capture dispatch for the CmdClearDepthStencilImage hook. Moved verbatim out of
// framebuffer.cpp so that hook stays a thin dispatcher. srcWidth/srcHeight are the
// source image's tracked dimensions (looked up by the hook).
void RND_Renderer::LinuxHandleDepthClear(OpenXR::EyeSide side,
        const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image,
        VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount,
        const VkImageSubresourceRange* pRanges, uint32_t srcWidth, uint32_t srcHeight) {
    const long frameIdx = pDepthStencil->stencil;
    if (srcWidth > 0 && srcHeight > 0) {
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        LinuxCopyDepthToSwapchain(side, commandBuffer, image, srcWidth, srcHeight, frameIdx);
        VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    pDispatch.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

// Called from CmdClearColorImage on Cemu's rendering thread
// Blits game content to intermediate texture (NOT directly to swapchain)
void RND_Renderer::LinuxCopyColorToSwapchain(OpenXR::EyeSide side, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx) {
    if (!s_linux || !s_linux->swapchainCreated) return;
    int sideIdx = (int)side;
    if (sideIdx < 0 || sideIdx >= 2 || frameIdx < 0 || frameIdx >= 2) {
        static int s_invalidColorFrameLogs = 0;
        if (s_invalidColorFrameLogs++ < 8) {
            Log::print<ERROR>("Linux color copy skipped invalid side/frame: side={} frame={}", sideIdx, frameIdx);
        }
        return;
    }

    int wIdx = (int)frameIdx;
    if (!s_linux->intermediateColor[sideIdx][wIdx]) return;
    if (s_linux->captureDiagnosticsEnabled) {
        s_linux->stats.diag3DColorCopies[sideIdx].fetch_add(1, std::memory_order_relaxed);
    }

    VkImage dstImage = s_linux->intermediateColor[sideIdx][wIdx]->GetImage();
    uint32_t dstW = s_linux->intermediateColor[sideIdx][wIdx]->GetWidth();
    uint32_t dstH = s_linux->intermediateColor[sideIdx][wIdx]->GetHeight();

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();

    // Transition intermediate to TRANSFER_DST
    s_linux->intermediateColor[sideIdx][wIdx]->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Compute UV transform to crop the 16:9 render to this eye's actual FOV
    float renderAspect = (float)srcWidth / (float)srcHeight;
    RenderUtils::UvTransform uv = ComputeEyeUvTransform(side, renderAspect);

    int32_t srcX0 = (int32_t)(uv.offsetX * srcWidth);
    int32_t srcY0 = (int32_t)(uv.offsetY * srcHeight);
    int32_t srcX1 = (int32_t)((uv.offsetX + uv.scaleX) * srcWidth);
    int32_t srcY1 = (int32_t)((uv.offsetY + uv.scaleY) * srcHeight);

    static int s_colorCopyLogs[2] = {};
    if (s_linux->captureDiagnosticsEnabled && sideIdx >= 0 && sideIdx < 2 && s_colorCopyLogs[sideIdx]++ < 6) {
        auto rawFov = GetFOV(side);
        if (rawFov.has_value()) {
            Log::print<INFO>(
                "Linux color copy eye={} magicFrame={} buffer={} src={}x{} dst={}x{} uv=({:.4f},{:.4f} {:.4f}x{:.4f}) crop=({},{})->({},{}) rawFov=({:.1f},{:.1f},{:.1f},{:.1f})",
                sideIdx, frameIdx, wIdx, srcWidth, srcHeight, dstW, dstH,
                uv.offsetX, uv.offsetY, uv.scaleX, uv.scaleY,
                srcX0, srcY0, srcX1, srcY1,
                glm::degrees(rawFov->angleLeft), glm::degrees(rawFov->angleRight),
                glm::degrees(rawFov->angleUp), glm::degrees(rawFov->angleDown));
        } else {
            Log::print<INFO>(
                "Linux color copy eye={} magicFrame={} buffer={} src={}x{} dst={}x{} uv=({:.4f},{:.4f} {:.4f}x{:.4f}) crop=({},{})->({},{}) rawFov=missing",
                sideIdx, frameIdx, wIdx, srcWidth, srcHeight, dstW, dstH,
                uv.offsetX, uv.offsetY, uv.scaleX, uv.scaleY,
                srcX0, srcY0, srcX1, srcY1);
        }
    }

    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.srcOffsets[0] = { srcX0, srcY0, 0 };
    blitRegion.srcOffsets[1] = { srcX1, srcY1, 1 };
    blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = { (int32_t)dstW, (int32_t)dstH, 1 };

    dispatch->CmdBlitImage(cmdBuffer,
        srcImage, VK_IMAGE_LAYOUT_GENERAL,
        dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_LINEAR);

    LinuxDesktopMirror::CaptureColor(sideIdx, cmdBuffer, srcImage, srcWidth, srcHeight, frameIdx);

    // Leave intermediate in TRANSFER_SRC for EndFrame to copy to swapchain
    s_linux->intermediateColor[sideIdx][wIdx]->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    auto& captured = s_linux->captured3DFrames[wIdx];
    std::array<XrView, 2> renderViewsSnapshot = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
    XrTime renderDisplayTimeSnapshot = 0;
    bool hasRenderViewsSnapshot = false;
    {
        std::shared_lock viewsLock(m_viewsMutex);
        if (m_currViews.has_value()) {
            renderViewsSnapshot = *m_currViews;
            renderDisplayTimeSnapshot = m_linuxCurrViewsDisplayTime;
            hasRenderViewsSnapshot = true;
        }
    }
    captured.renderViews = renderViewsSnapshot;
    captured.renderDisplayTime = renderDisplayTimeSnapshot;
    captured.hasRenderViews = hasRenderViewsSnapshot;
    captured.eyeCopied[sideIdx] = true;
    captured.depthCopied[sideIdx] = false;
    captured.colorCopyAt[sideIdx] = std::chrono::high_resolution_clock::now();
    captured.complete = captured.eyeCopied[0] && captured.eyeCopied[1];

    auto copyCurrentRenderSnapshotToPair = [&](LinuxRenderState::Captured3DPair& pair) {
        if (hasRenderViewsSnapshot) {
            pair.renderViews = renderViewsSnapshot;
            pair.renderDisplayTime = renderDisplayTimeSnapshot;
            pair.hasRenderViews = true;
        }
    };

    auto logPauseCopyDiag = [&](const char* event) {
        if (!s_linux->captureDiagnosticsEnabled) return;
        int remaining = s_linux->pauseDiag.verbose3DCopiesRemaining.load(std::memory_order_acquire);
        while (remaining > 0) {
            if (s_linux->pauseDiag.verbose3DCopiesRemaining.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                const auto& pending = s_linux->pending3DPair;
                const auto& completed = s_linux->completed3DPair;
                XrTime currViewsDisplayTime = 0;
                {
                    std::shared_lock viewsLock(m_viewsMutex);
                    currViewsDisplayTime = m_linuxCurrViewsDisplayTime;
                }
                Log::print<INFO>(
                    "Linux pause 3D copy diag event={} epoch={} inPause={} eye={} magicFrame={} buffer={} capturedEye=({}, {}) capturedDepth=({}, {}) capturedComplete={} capturedPose={} capturedDelta={:.2f}ms pending=({},{} idx={}, {}) completed=({},{} idx={}, {}) frameHasContent={}",
                    event,
                    s_linux->pauseDiag.epoch.load(std::memory_order_acquire),
                    s_linux->pauseDiag.inInventoryPause.load(std::memory_order_acquire) ? "yes" : "no",
                    sideIdx,
                    frameIdx,
                    wIdx,
                    captured.eyeCopied[0] ? "yes" : "no",
                    captured.eyeCopied[1] ? "yes" : "no",
                    captured.depthCopied[0] ? "yes" : "no",
                    captured.depthCopied[1] ? "yes" : "no",
                    captured.complete ? "yes" : "no",
                    captured.hasRenderViews ? "yes" : "no",
                    LinuxDisplayTimeDeltaMs(captured.renderDisplayTime, currViewsDisplayTime),
                    pending.eyeCopied[0] ? "yes" : "no",
                    pending.eyeCopied[1] ? "yes" : "no",
                    pending.bufferIdx[0],
                    pending.bufferIdx[1],
                    completed.eyeCopied[0] ? "yes" : "no",
                    completed.eyeCopied[1] ? "yes" : "no",
                    completed.bufferIdx[0],
                    completed.bufferIdx[1],
                    s_linux->frameHasContent ? "yes" : "no");
                break;
            }
        }
    };

    if (sideIdx == 0) {
        auto& pair = s_linux->pending3DPair;
        pair.Reset();
        pair.eyeCopied[0] = true;
        pair.bufferIdx[0] = wIdx;
        pair.colorCopyAt[0] = captured.colorCopyAt[0];
        copyCurrentRenderSnapshotToPair(pair);
        if (s_linux->captureDiagnosticsEnabled) {
            s_linux->stats.diag3DPendingLeftStarts.fetch_add(1, std::memory_order_relaxed);
        }
        logPauseCopyDiag("left-start");
        return;
    }

    auto& pair = s_linux->pending3DPair;
    if (!pair.eyeCopied[0]) {
        if (s_linux->captureDiagnosticsEnabled) {
            s_linux->stats.diag3DOrphanRightDrops.fetch_add(1, std::memory_order_relaxed);
        }
        logPauseCopyDiag("orphan-right");
        static int s_orphanRightLogs = 0;
        if (s_linux->captureDiagnosticsEnabled && s_orphanRightLogs++ < 8) {
            Log::print<INFO>("Linux 3D right-eye capture dropped while waiting for left eye: magicFrame={} buffer={}", frameIdx, wIdx);
        }
        return;
    }

    pair.eyeCopied[1] = true;
    pair.bufferIdx[1] = wIdx;
    pair.colorCopyAt[1] = captured.colorCopyAt[1];
    s_linux->completed3DPair = pair;
    s_linux->frameHasContent = true;
    if (s_linux->captureDiagnosticsEnabled) {
        s_linux->stats.diag3DCompletedPairs.fetch_add(1, std::memory_order_relaxed);
    }
    logPauseCopyDiag("pair-complete");

    static int s_completed3DLogs = 0;
    if (s_linux->captureDiagnosticsEnabled && s_completed3DLogs++ < 12) {
        Log::print<INFO>("Linux 3D observed pair complete buffers=({}, {}) eyeDelta={:.3f}ms depth=({}, {}) renderPose={}",
            s_linux->completed3DPair.bufferIdx[0],
            s_linux->completed3DPair.bufferIdx[1],
            std::chrono::duration<double, std::milli>(
                s_linux->completed3DPair.colorCopyAt[1] - s_linux->completed3DPair.colorCopyAt[0]).count(),
            s_linux->captured3DFrames[s_linux->completed3DPair.bufferIdx[0]].depthCopied[0] ? "yes" : "no",
            s_linux->captured3DFrames[s_linux->completed3DPair.bufferIdx[1]].depthCopied[1] ? "yes" : "no",
            s_linux->completed3DPair.hasRenderViews ? "yes" : "no");
    }
    pair.Reset();
}

void RND_Renderer::LinuxCopyDepthToSwapchain(OpenXR::EyeSide side, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx) {
    if (!s_linux || !s_linux->swapchainCreated) return;
    int sideIdx = (int)side;
    if (sideIdx < 0 || sideIdx >= 2 || frameIdx < 0 || frameIdx >= 2) {
        static int s_invalidDepthFrameLogs = 0;
        if (s_invalidDepthFrameLogs++ < 8) {
            Log::print<ERROR>("Linux depth copy skipped invalid side/frame: side={} frame={}", sideIdx, frameIdx);
        }
        return;
    }

    int wIdx = (int)frameIdx;
    if (!s_linux->intermediateDepth[sideIdx][wIdx]) return;
    if (s_linux->captureDiagnosticsEnabled) {
        s_linux->stats.diag3DDepthCopies[sideIdx].fetch_add(1, std::memory_order_relaxed);
    }

    VkImage dstImage = s_linux->intermediateDepth[sideIdx][wIdx]->GetImage();
    uint32_t dstW = s_linux->intermediateDepth[sideIdx][wIdx]->GetWidth();
    uint32_t dstH = s_linux->intermediateDepth[sideIdx][wIdx]->GetHeight();
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();

    // Transition intermediate to TRANSFER_DST
    s_linux->intermediateDepth[sideIdx][wIdx]->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Depth must use the same source sub-FOV as color. Otherwise the runtime's
    // depth reprojection sees depth values for a different projection than the
    // color image, which shows up as head-tracking judder.
    float renderAspect = (float)srcWidth / (float)srcHeight;
    RenderUtils::UvTransform uv = ComputeEyeUvTransform(side, renderAspect);

    int32_t srcX0 = (int32_t)(uv.offsetX * srcWidth);
    int32_t srcY0 = (int32_t)(uv.offsetY * srcHeight);
    int32_t srcX1 = (int32_t)((uv.offsetX + uv.scaleX) * srcWidth);
    int32_t srcY1 = (int32_t)((uv.offsetY + uv.scaleY) * srcHeight);

    static int s_depthCopyLogs[2] = {};
    if (s_linux->captureDiagnosticsEnabled && sideIdx >= 0 && sideIdx < 2 && s_depthCopyLogs[sideIdx]++ < 4) {
        Log::print<INFO>(
            "Linux depth copy eye={} magicFrame={} buffer={} src={}x{} dst={}x{} uv=({:.4f},{:.4f} {:.4f}x{:.4f}) crop=({},{})->({},{})",
            sideIdx, frameIdx, wIdx, srcWidth, srcHeight, dstW, dstH,
            uv.offsetX, uv.offsetY, uv.scaleX, uv.scaleY,
            srcX0, srcY0, srcX1, srcY1);
    }

    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    blitRegion.srcOffsets[0] = { srcX0, srcY0, 0 };
    blitRegion.srcOffsets[1] = { srcX1, srcY1, 1 };
    blitRegion.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = { (int32_t)dstW, (int32_t)dstH, 1 };

    dispatch->CmdBlitImage(cmdBuffer,
        srcImage, VK_IMAGE_LAYOUT_GENERAL,
        dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_NEAREST);

    // Leave in TRANSFER_SRC for EndFrame
    s_linux->intermediateDepth[sideIdx][wIdx]->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    s_linux->captured3DFrames[wIdx].depthCopied[sideIdx] = true;
}

void RND_Renderer::LinuxCopyHudToSwapchain(VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight) {
    static uint32_t hudCopyCount = 0;
    hudCopyCount++;
    if (!s_linux) return;

    // Lazily create BOTH HUD intermediate buffers (double-buffered).
    for (int b = 0; b < 2; b++) {
        if (!s_linux->intermediateHud[b]) {
            s_linux->intermediateHud[b] = new VulkanTexture(srcWidth, srcHeight,
                VK_FORMAT_R8G8B8A8_SRGB,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
    }
    if (hudCopyCount == 1) {
        Log::print<INFO>("Created HUD intermediate {}x{} (double-buffered)", srcWidth, srcHeight);
    }
    if (s_linux->hudSwapchain.handle == XR_NULL_HANDLE) {
        s_linux->hudSwapchain.Create(m_session, srcWidth, srcHeight,
            VK_FORMAT_R8G8B8A8_SRGB,
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
        Log::print<INFO>("Created HUD swapchain {}x{}", srcWidth, srcHeight);
    }

    int wIdx = s_linux->currentWriteIdx_hud;
    auto* hudBuf = s_linux->intermediateHud[wIdx];
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    hudBuf->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.srcOffsets[0] = { 0, 0, 0 };
    blitRegion.srcOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = { (int32_t)hudBuf->GetWidth(), (int32_t)hudBuf->GetHeight(), 1 };

    dispatch->CmdBlitImage(cmdBuffer,
        srcImage, VK_IMAGE_LAYOUT_GENERAL,
        hudBuf->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_LINEAR);

    LinuxDesktopMirror::CaptureHud(cmdBuffer, srcImage, srcWidth, srcHeight, wIdx);

    hudBuf->vkTransitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (s_linux->captureDiagnosticsEnabled && (hudCopyCount <= 5 || hudCopyCount % 500 == 0)) {
        Log::print<INFO>("LinuxCopyHudToSwapchain #{}: src={}x{} → intermediate[{}]", hudCopyCount, srcWidth, srcHeight, wIdx);
    }
    s_linux->hudCopied = true;
}

// Phase 5+6: called by GetPoses/GetPose/GetFOV before reading m_currViews.
// Atomically claims the per-frame latch; the winner waits for the submit thread
// to have pendingViews ready and copies them into m_currViews (with height
// offset). Losers see the latch already done and proceed (their shared_lock
// will wait for the winner's unique_lock to release in the caller).
void RND_Renderer::LatchPendingIfNeeded() const {
    if (!s_linux) return;
    // Fast path: latch already done for this Cemu frame.
    if (!m_linuxNeedsLatch.exchange(false, std::memory_order_acquire)) return;

    // If the submit thread has exited (fatal error), don't bother waiting for
    // new pose data — it'll never come. Proceed with whatever m_currViews
    // currently holds. Cemu can keep running, just without VR re-presentation.
    if (!s_linux->submit.alive.load(std::memory_order_acquire)) return;

    // Wait for submit thread to have prepared pendingViews newer than what we
    // consumed last. In steady state this is already true (submit thread is
    // much faster than Cemu's render), so the wait is essentially free.
    uint64_t consumed = m_linuxConsumedFrameId.load(std::memory_order_acquire);
    std::array<XrView, 2> views;
    XrTime viewsDisplayTime = 0;
    {
        std::unique_lock<std::mutex> lock(s_linux->submit.pendingMutex);
        // Bounded wait so we don't hang Cemu forever if submit thread is stuck.
        bool got = s_linux->submit.pendingCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return s_linux->submit.pendingFrameId.load(std::memory_order_acquire) > consumed
                && s_linux->submit.pendingViews.has_value();
        });
        if (!got) {
            static int s_warnCount = 0;
            if (s_warnCount++ < 10) {
                Log::print<ERROR>("LatchPendingIfNeeded timed out (consumed={}, pending={}, hasViews={}, submitIter={}, submitStep={})",
                    consumed,
                    s_linux->submit.pendingFrameId.load(std::memory_order_acquire),
                    s_linux->submit.pendingViews.has_value(),
                    s_linux->submit.iterCount.load(std::memory_order_relaxed),
                    s_linux->submit.currentStep.load(std::memory_order_relaxed));
            }
            return; // proceed with stale m_currViews
        }
        views = *s_linux->submit.pendingViews;
        viewsDisplayTime = s_linux->submit.pendingFrameState.predictedDisplayTime;
        m_linuxConsumedFrameId.store(s_linux->submit.pendingFrameId.load(std::memory_order_acquire),
                                     std::memory_order_release);
    }

    // Apply seated height offset and publish to m_currViews. This is the pose
    // used by the game camera and therefore the pose to attach to captured 3D
    // content for projection-layer reprojection.
    {
        std::unique_lock viewsLock(m_viewsMutex);
        constexpr float kTargetHeadHeight = 1.65f;
        float currentHeight = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        float heightOffset = kTargetHeadHeight - currentHeight;
        for (auto& v : views) v.pose.position.y += heightOffset;
        m_currViews = views;
        m_linuxCurrViewsDisplayTime = viewsDisplayTime;
    }
}

// Backend-specific view accessor used by the shared pose getters in renderer.h.
// Late-latches the submit thread's pending pose (once per Cemu frame) and returns
// the current views under the views lock. On Windows this is a trivial inline read
// (see renderer.h) — the mutex/latch exist only on Linux.
std::optional<std::array<XrView, 2>> RND_Renderer::CurrentViews() const {
    LatchPendingIfNeeded();
    std::shared_lock viewsLock(m_viewsMutex);
    return m_currViews;
}

std::optional<std::array<XrView, 2>> RND_Renderer::UpdateViews(XrTime predictedDisplayTime) {
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = VRManager::instance().XR->m_stageSpace;

    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    uint32_t viewCount = 2;
    std::array<XrView, 2> views = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };

    XrResult result = xrLocateViews(m_session, &locateInfo, &viewState, viewCount, &viewCount, views.data());
    if (XR_FAILED(result) || viewCount != 2) {
        return std::nullopt;
    }

    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        return std::nullopt;
    }

    {
        std::unique_lock viewsLock(m_viewsMutex);
        m_currViews = views;
        m_linuxCurrViewsDisplayTime = predictedDisplayTime;
    }

    static int viewLogCount = 0;
    if (++viewLogCount <= 3) {
        for (int eye = 0; eye < 2; eye++) {
            Log::print<INFO>("Eye {} pose: pos=({:.3f},{:.3f},{:.3f}) fov=(L={:.2f} R={:.2f} U={:.2f} D={:.2f})",
                eye,
                views[eye].pose.position.x, views[eye].pose.position.y, views[eye].pose.position.z,
                glm::degrees(views[eye].fov.angleLeft), glm::degrees(views[eye].fov.angleRight),
                glm::degrees(views[eye].fov.angleUp), glm::degrees(views[eye].fov.angleDown));
        }
        float ipd = views[1].pose.position.x - views[0].pose.position.x;
        Log::print<INFO>("IPD: {:.4f}m ({:.1f}mm)", ipd, ipd * 1000.0f);
    }

    return views;
}

// Stubs for ImGuiOverlay methods (full implementation in vulkan_imgui.cpp, Win32-only for now)
RND_Renderer::ImGuiOverlay::~ImGuiOverlay() {
}

void RND_Renderer::ImGuiOverlay::ProcessInputs(OpenXR::InputState& inputs, const VPADStatus& vpadStatus) {
    (void)inputs;
    (void)vpadStatus;
}

#endif // !_WIN32
