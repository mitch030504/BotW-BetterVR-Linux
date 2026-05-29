#pragma once
// Linux-only capture diagnostics for the Vulkan framebuffer hooks. Env-gated by
// BETTERVR_LINUX_CAPTURE_DIAG. Extracted from framebuffer.cpp to keep the shared hook
// file lean; compiled only on Linux (see src/CMakeLists.txt). Declared in namespace
// VRLayer so framebuffer.cpp's existing unqualified call sites resolve via its
// `using namespace VRLayer`.

#include "rendering/openxr.h"

class RND_Renderer;

namespace VRLayer {

// True when BETTERVR_LINUX_CAPTURE_DIAG selects verbose capture diagnostics.
bool LinuxCaptureDiagnosticsEnabled();

// Tracks inventory/pause-menu open<->close transitions (diagnostic marker only).
// Returns +1 on open, -1 on close, 0 otherwise.
int UpdateLinuxInventoryPauseTransition();

// Logs the inventory-pause detector state (screen flags, capture state, etc.).
void LogLinuxInventoryPauseDetectorState(const char* event, RND_Renderer* renderer,
    OpenXR::EyeSide side, long frameIdx, VkFormat srcFormat, uint32_t srcWidth, uint32_t srcHeight);

}
