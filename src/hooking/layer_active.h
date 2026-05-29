#pragma once

namespace VRLayer {
    // True when the BetterVR layer should be active in the current host process
    // (i.e. the process is Cemu). Cross-platform; the result is cached on first call.
    // Lives in its own translation unit because it is called as an early-out from
    // many hook overrides across layer.cpp, framebuffer.cpp, and vulkan.cpp.
    bool IsLayerActiveForProcess();
}
