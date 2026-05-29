// Windows / D3D12 backend members of RND_Renderer.
// Included into the RND_Renderer class body (public section) from renderer.h under
// #ifdef _WIN32. Definitions live in renderer.cpp. This is a pure relocation of what
// were inline #ifdef _WIN32 members — no logic changes.

// Current head-pose views accessor used by the shared pose getters. Windows updates
// the views synchronously, so this is a trivial read — no lock, no latch.
std::optional<std::array<XrView, 2>> CurrentViews() const { return m_currViews; }

class Layer3D {
public:
    explicit Layer3D(VkExtent2D inputRes, VkExtent2D outputRes);
    ~Layer3D();

    SharedTexture* CopyColorToLayer(OpenXR::EyeSide side, VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
    SharedTexture* CopyDepthToLayer(OpenXR::EyeSide side, VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
    void PrepareRendering(OpenXR::EyeSide side);
    void StartRendering();
    void Render(OpenXR::EyeSide side, long frameIdx, SharedTexture* fadeTexture, const DebugDrawRenderData& debugDrawData);
    const std::array<XrCompositionLayerProjectionView, 2>& FinishRendering(long frameIdx);

    float GetAspectRatio(OpenXR::EyeSide side) const { return m_recommendedAspectRatios[side]; }
    const RenderUtils::UvTransform& GetPresentUvTransform(OpenXR::EyeSide side) const { return m_presentUvTransforms[side]; }
    long GetCurrentFrameIdx() const { return m_currentFrameIdx; }
    auto& GetSharedTextures() { return m_textures; }
    auto& GetDepthSharedTextures() { return m_depthTextures; }

private:
    std::array<std::unique_ptr<Swapchain<DXGI_FORMAT_R8G8B8A8_UNORM_SRGB>>, 2> m_swapchains;
    std::array<std::unique_ptr<Swapchain<DXGI_FORMAT_D32_FLOAT>>, 2> m_depthSwapchains;
    std::array<std::unique_ptr<RND_D3D12::PresentPipeline<true>>, 2> m_presentPipelines;
    std::array<std::array<std::unique_ptr<SharedTexture>, 2>, 2> m_textures;
    std::array<std::array<std::unique_ptr<SharedTexture>, 2>, 2> m_depthTextures;
    std::array<float, 2> m_recommendedAspectRatios = { 1.0f, 1.0f };
    std::array<RenderUtils::UvTransform, 2> m_presentUvTransforms = {};
    std::unique_ptr<RND_D3D12::DebugDrawPipeline> m_debugDrawPipeline;

    std::array<XrCompositionLayerProjectionView, 2> m_projectionViews = {};
    std::array<XrCompositionLayerDepthInfoKHR, 2> m_projectionViewsDepthInfo = {};

    long m_currentFrameIdx = 0;
};

class Layer2D {
public:
    explicit Layer2D(VkExtent2D inputRes, VkExtent2D outputRes);
    ~Layer2D();

    SharedTexture* CopyColorToLayer(VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
    // AMD GPU FIX: With incrementing values, Vulkan signals odd values (1,3,5...), D3D12 signals even values (2,4,6...)
    // Texture is ready for D3D12 when Vulkan has signaled (odd value > 0)
    bool IsTextureReady(long frameIdx) const {
        uint64_t lastSignal = m_textures[frameIdx]->GetLastSignalledValue();
        return lastSignal > 0 && (lastSignal % 2 == 1);
    };
    void StartRendering() const;
    void Render(long frameIdx);
    std::vector<XrCompositionLayerQuad> FinishRendering(XrTime predictedDisplayTime, long frameIdx);
    long GetCurrentFrameIdx() const { return m_currentFrameIdx; }
    auto& GetSharedTextures() { return m_textures; }

    static void SetBowAimingActive(bool isActive) { s_isBowAimingActive = isActive; }
    static bool IsBowAimingActive() { return s_isBowAimingActive; }

private:
    static std::atomic_bool s_isBowAimingActive;
    std::unique_ptr<Swapchain<DXGI_FORMAT_R8G8B8A8_UNORM_SRGB>> m_swapchain;
    std::unique_ptr<RND_D3D12::PresentPipeline<false>> m_presentPipeline;
    std::array<std::unique_ptr<SharedTexture>, 2> m_textures;

    glm::quat m_currentOrientation = glm::identity<glm::fquat>();

    long m_currentFrameIdx = 0;
};

std::unique_ptr<Layer3D> m_layer3D;
std::unique_ptr<Layer2D> m_layer2D;
