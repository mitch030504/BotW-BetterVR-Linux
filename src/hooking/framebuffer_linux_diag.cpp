#include "pch.h"
#include "framebuffer_linux_diag.h"

#include "instance.h"
#include "rendering/renderer.h"

#include <cstdlib>
#include <cstring>

// Linux capture diagnostics extracted verbatim from framebuffer.cpp. Env-gated and
// diagnostic-only; not on the capture critical path.

namespace VRLayer {

namespace {
    bool s_linuxInventoryPauseWasOpen = false;

    bool IsLinuxInventoryPauseStartVisible() {
        return CemuHooks::IsScreenVisible(ScreenId::PauseMenu_00)
            || CemuHooks::IsScreenVisible(ScreenId::PauseMenuInfo_00);
    }

    const char* YesNo(bool value) {
        return value ? "yes" : "no";
    }

    std::string FormatLinuxScreenState(ScreenId screen) {
        const auto state = CemuHooks::GetScreenDebugState(screen);
        return std::format("{}={:08X}/p{}/s{}/{}{}",
            ScreenIdToString(screen),
            state.ptr,
            static_cast<int>(state.openPriority),
            static_cast<int>(state.state),
            state.open ? "O" : "-",
            state.visible ? "V" : "-");
    }

    std::string FormatLinuxPauseScreenStates() {
        return std::format("{} {} {} {} {} {} {}",
            FormatLinuxScreenState(ScreenId::PauseMenuBG_00),
            FormatLinuxScreenState(ScreenId::PauseMenu_00),
            FormatLinuxScreenState(ScreenId::PauseMenuInfo_00),
            FormatLinuxScreenState(ScreenId::AppMap_00),
            FormatLinuxScreenState(ScreenId::AppHome_00),
            FormatLinuxScreenState(ScreenId::MainShortCut_00),
            FormatLinuxScreenState(ScreenId::MessageTipsPauseMenu_00));
    }
}

bool LinuxCaptureDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BETTERVR_LINUX_CAPTURE_DIAG");
        if (value == nullptr || value[0] == '\0') return false;
        return std::strcmp(value, "0") != 0
            && std::strcmp(value, "false") != 0
            && std::strcmp(value, "FALSE") != 0
            && std::strcmp(value, "no") != 0
            && std::strcmp(value, "NO") != 0;
    }();
    return enabled;
}

int UpdateLinuxInventoryPauseTransition() {
    // Diagnostic transition marker only. The 3D capture pair state must remain
    // self-synchronizing from the actual left/right clear sequence; resetting it
    // from menu visibility races the game's pause teardown and creates orphans.
    const bool inventoryPauseOpen = IsLinuxInventoryPauseStartVisible();

    if (inventoryPauseOpen) {
        if (!s_linuxInventoryPauseWasOpen) {
            s_linuxInventoryPauseWasOpen = true;
            return 1;
        }
        return 0;
    }

    if (s_linuxInventoryPauseWasOpen) {
        s_linuxInventoryPauseWasOpen = false;
        return -1;
    }
    return 0;
}

void LogLinuxInventoryPauseDetectorState(
    const char* event,
    RND_Renderer* renderer,
    OpenXR::EyeSide side,
    long frameIdx,
    VkFormat srcFormat,
    uint32_t srcWidth,
    uint32_t srcHeight) {
    Log::print<INFO>(
        "Linux inventory pause {} detector: framesSinceCamera={} inGame={} showingMenu={} monoPulse={} gameCapture={} side={} magicFrame={} src={}x{} fmt={} screens: {}",
        event,
        CemuHooks::GetFramesSinceLastCameraUpdate(),
        YesNo(CemuHooks::IsInGame()),
        YesNo(CemuHooks::IsShowingMenu()),
        YesNo(CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures()),
        YesNo(renderer != nullptr && renderer->IsGameCapturing3DFrameBuffer()),
        side == OpenXR::EyeSide::LEFT ? "L" : "R",
        frameIdx,
        srcWidth,
        srcHeight,
        srcFormat,
        FormatLinuxPauseScreenStates());
}

}
