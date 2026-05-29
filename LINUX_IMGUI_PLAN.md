# Linux ImGui Integration Plan

## Current Status

The first Linux ImGui wiring attempt has been backed out because it still caused an immediate Cemu crash. The stable Linux Vulkan/OpenXR path should remain the default while this work is paused.

The reverted attempt tried to reuse the Windows `ImGuiOverlay` implementation directly from the Linux HUD capture path. That exposed several assumptions that are unsafe or at least unproven on Linux:

- The overlay was constructed inside the `CmdClearColorImage` HUD hook while Cemu was recording its own command buffer.
- ImGui's Vulkan backend uploads fonts with its own `vkQueueSubmit` and `vkQueueWaitIdle`.
- The Windows overlay assumes a single-threaded ImGui context, Win32 input/window state, and D3D12-era texture/lifetime behavior.
- The Linux path was modifying the captured HUD source image before the normal HUD copy completed.

Even after moving font upload toward the dedicated VR queue and adding locks, the crash remained. That suggests the next pass should not be another small patch on top of the direct Windows port.

## Re-Entry Approach

1. Keep Linux ImGui disabled by default.

   Add an explicit development flag, for example `BETTERVR_LINUX_IMGUI=1`, before wiring any overlay code back into the frame path. A normal Linux beta launch must continue to use the currently stable HUD and 3D presentation path.

2. Split initialization from Vulkan hook callbacks.

   Do not create the ImGui Vulkan backend, upload fonts, create help textures, or submit/wait a queue from `CmdClearColorImage`. Initialize GPU resources from renderer-owned code where we control the command pool, queue, locking, and failure logging.

3. Give Linux a small overlay backend instead of directly reusing the Windows one.

   Reuse the menu drawing code in `hooking/imgui_menus.cpp`, but make the platform/backend layer Linux-specific:

   - no Win32 cursor/window calls
   - no D3D12 assumptions
   - no implicit render target format assumptions
   - explicit ownership of the ImGui context thread
   - explicit teardown order for ImGui descriptors, textures, render pass, and Vulkan backend state

4. Avoid mutating Cemu's source images in the first working version.

   The safest first version should leave Cemu-owned source images alone. Prefer composing into BetterVR-owned intermediate or swapchain images using BetterVR-owned command buffers. If drawing from inside the Cemu command buffer is still needed, keep it to simple copies/barriers after the overlay resources are already initialized and proven.

5. Stage the feature in narrow test modes.

   Bring pieces online independently:

   - flag enabled, no ImGui initialization
   - ImGui context and CPU menu draw only
   - Vulkan backend init and font upload only, outside the hook
   - render an empty transparent overlay into a BetterVR-owned target
   - render FPS overlay text
   - enable menu input
   - enable the full settings/help menu

6. Add targeted diagnostics before the next test build.

   Log one line for each lifecycle phase:

   - feature flag state
   - chosen queue family/index and whether it is shared with Cemu, runtime, or copy work
   - render target format and required format feature support
   - command pool and command buffer owner
   - ImGui backend init success
   - font/help texture upload success
   - first overlay draw
   - teardown start/end

   Keep these logs one-shot so they help diagnose startup crashes without flooding frame timing logs.

## Suggested Next Implementation Slice

The next code slice should be intentionally boring:

1. Add the Linux ImGui feature flag and one-shot logging.
2. Compile the full ImGui source on Linux again, but keep the runtime path disabled unless the flag is set.
3. Create a Linux-only overlay class or wrapper that owns only the ImGui context and input state, with no Vulkan backend work yet.
4. Prove that opening/closing Cemu with the flag disabled and enabled both remain stable.
5. Only then add Vulkan backend initialization outside the HUD capture hook.

This keeps the working beta path protected while still letting us bisect the ImGui crash by subsystem instead of reintroducing the whole overlay at once.
