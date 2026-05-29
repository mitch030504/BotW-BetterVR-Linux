// Linux-only RND_Vulkan three-queue topology accessors + state. Included at the END of
// the RND_Vulkan class body from vulkan.h under #ifndef _WIN32 (the trailing access
// specifier is therefore harmless). queue 0 = Cemu, 1 = OpenXR runtime, 2 = our copies.
public:
    uint32_t GetQueueFamilyIndex() const { return m_queueFamilyIndex; }
    uint32_t GetQueueIndex() const { return m_queueIndex; }
    void SetQueueInfo(uint32_t familyIndex, uint32_t queueIndex) { m_queueFamilyIndex = familyIndex; m_queueIndex = queueIndex; }

    // Dedicated submit queue for VR work (OpenXR runtime queue, passed via xrCreateSession).
    // -1 if our CreateDevice hook couldn't allocate a second queue from Cemu's graphics
    // family (we fall back to sharing Cemu's queue with a mutex).
    int32_t GetSubmitQueueIndex() const { return m_submitQueueIndex; }
    void SetSubmitQueueIndex(int32_t idx) { m_submitQueueIndex = idx; }
    bool HasDedicatedSubmitQueue() const { return m_submitQueueIndex >= 0; }

    // Copy queue: used by the submit thread for intermediate->swapchain copies, kept
    // distinct from the runtime queue so compositor and copy work don't pile up.
    int32_t GetCopyQueueIndex() const { return m_copyQueueIndex; }
    void SetCopyQueueIndex(int32_t idx) { m_copyQueueIndex = idx; }
    bool HasDedicatedCopyQueue() const { return m_copyQueueIndex >= 0; }

private:
    uint32_t m_queueFamilyIndex = 0;
    uint32_t m_queueIndex = 0;
    int32_t m_submitQueueIndex = -1;  // OpenXR runtime queue (queue 1)
    int32_t m_copyQueueIndex = -1;    // Our submit-thread copy queue (queue 2)
