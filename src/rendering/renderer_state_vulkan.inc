// Linux / Vulkan backend protected state of RND_Renderer.
// Included into the RND_Renderer class body (protected section) from renderer.h under
// #ifndef _WIN32. Pure relocation of what were inline #ifndef _WIN32 members.

mutable std::shared_mutex m_viewsMutex; // protects m_currViews (submit thread + Cemu thread)

// Per-Cemu-frame latch flag: set true at frame start; the first CurrentViews call
// that exchanges it back to false performs the pendingViews -> m_currViews latch.
mutable std::atomic_bool m_linuxNeedsLatch = false;
mutable std::atomic<uint64_t> m_linuxConsumedFrameId{0};
// The OpenXR predicted display time the latched views were located for (guarded by m_viewsMutex).
mutable XrTime m_linuxCurrViewsDisplayTime = 0;
