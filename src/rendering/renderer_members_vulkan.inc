// Linux / Vulkan backend members of RND_Renderer.
// Included into the RND_Renderer class body (public section) from renderer.h under
// #ifndef _WIN32. Definitions live in renderer_vulkan.cpp. This is a pure relocation
// of what were inline #ifndef _WIN32 members — no logic changes.

// Cemu framebuffer capture entry points, driven by the clear hooks in framebuffer.cpp.
void LinuxCopyColorToSwapchain(OpenXR::EyeSide side, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx);
void LinuxCopyDepthToSwapchain(OpenXR::EyeSide side, VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight, long frameIdx);
void LinuxCopyHudToSwapchain(VkCommandBuffer cmdBuffer, VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight);
void LinuxMarkInventoryPauseTransition(bool opened);

void LinuxUpdateRuntimeFrameTiming(const XrFrameState& frameState, double waitFrameMs);

// Current head-pose views accessor used by the shared pose getters; late-latches the
// submit thread's pendingViews under the views lock (defined in renderer_vulkan.cpp).
std::optional<std::array<XrView, 2>> CurrentViews() const;
// Atomically claims the per-Cemu-frame latch and copies the submit thread's
// pendingViews into m_currViews (with seated height offset). Called by StartFrame
// and CurrentViews.
void LatchPendingIfNeeded() const;
