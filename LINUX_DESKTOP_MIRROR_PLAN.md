# Linux Desktop Mirror Plan

## Goal

Add a Linux desktop mirror mode so OBS or another screen recorder can capture BetterVR gameplay from Cemu's normal window while the headset continues to receive the existing low-latency OpenXR path.

The mirror must be optional, stable, and cheap enough to leave enabled while recording a demo. It should not change the headset path, pose timing, pause recovery, or the submit thread's native-HMD-rate loop.

## Current Rendering Baseline

Linux currently has two separate timing domains:

- **Cemu render thread**: captures 3D and HUD images from `CmdClearColorImage`, copies them into BetterVR-owned double-buffered Vulkan intermediates, then signals a timeline semaphore from `EndFrame`.
- **OpenXR submit thread**: waits for new published content, copies intermediates into OpenXR swapchains on the VR copy queue, then re-presents at HMD cadence with fresh poses.

The mirror should use the content Cemu already captured, but it should not read from or modify OpenXR swapchain images. Those images are runtime-owned and paced by headset presentation, not desktop presentation.

## Proposed Design

Use Cemu's existing Vulkan window swapchain as the mirror output. At `QueuePresentKHR`, when mirroring is enabled, insert one GPU submission before the real present:

1. Wait on Cemu's original present wait semaphores.
2. Record a small command buffer that writes the selected mirror image into the swapchain image Cemu is about to present.
3. Signal a BetterVR-owned present semaphore.
4. Call the real `QueuePresentKHR` with the same swapchain/image index, but waiting on the BetterVR semaphore instead of Cemu's original wait semaphores.

This keeps the work GPU-only, avoids CPU readback, and runs only at Cemu's desktop present cadence rather than HMD cadence.

## Implemented Slice

The first implementation covers the transfer-only MVP:

- `BETTERVR_DESKTOP_MIRROR=1` enables mirroring.
- `BETTERVR_DESKTOP_MIRROR_EYE=right|left` chooses the monoscopic 3D source eye; the default is right.
- Cemu swapchains are tracked through `CreateSwapchainKHR`, `GetSwapchainImagesKHR`, and `DestroySwapchainKHR`.
- The layer requests `VK_IMAGE_USAGE_TRANSFER_DST_BIT` for the Cemu swapchain when the surface supports it.
- Selected-eye 3D and HUD fallback sources are copied into BetterVR-owned mirror textures during existing capture hooks.
- `QueuePresentKHR` inserts a pre-present blit into the Cemu swapchain image and preserves Cemu's original present waits by replacing them with a BetterVR-owned semaphore signaled by the mirror submit.

This does not yet implement the full 3D+alpha-HUD composite shader, side-by-side mode, or mirror-specific timing stats.

## Source Image Strategy

Do not sample or relayout the existing VR intermediates from the desktop mirror pass. The submit thread may be reading those same images for VR, and changing their layout from the present queue would create avoidable cross-queue hazards.

Instead, create mirror-owned source textures when mirror mode is enabled:

- `mirrorColor[2]`, double-buffered, format `VK_FORMAT_R8G8B8A8_SRGB` or another format selected from the Cemu swapchain format.
- `mirrorHud[2]`, double-buffered, matching the HUD capture size or the mirror output size.
- optional `mirrorDepth[2]` later, only for debug visualizations.

During the existing Cemu command-buffer capture hooks:

- On the selected eye color capture, blit/copy the source image into the current mirror color texture.
- On HUD capture, blit/copy the HUD source into the current mirror HUD texture.
- Publish mirror read indices alongside the existing content publication.

Default mirror eye should be **right eye**, matching the Windows desktop path's preference. Add an env setting for `left`, `right`, or `side-by-side` later.

## Composition

### MVP

Start with a transfer-only MVP:

- Copy/blit the selected eye's 3D mirror texture into the Cemu swapchain image.
- If no 3D is available, copy/blit the HUD texture.
- Preserve aspect ratio with letterboxing/pillarboxing.

This is enough to prove swapchain tracking, synchronization, and pre-present submission without adding a Vulkan graphics pipeline.

### Demo-Ready Version

Add a small Vulkan fullscreen pass that composites:

- 3D mirror color
- HUD with alpha
- optional fade color/amount if needed

This pass should render directly into the Cemu swapchain image as a color attachment. It should use a single fullscreen triangle, one descriptor set for the source textures, and one small uniform block for UV transforms, aspect policy, and mode flags.

Shader handling options:

- Preferred: add GLSL shader sources and compile to SPIR-V as part of the Nix/CMake build.
- Acceptable for a first pass: check in small generated SPIR-V byte arrays for the mirror shaders.

## Swapchain Tracking

Extend the Vulkan layer hooks to track Cemu's desktop swapchains:

- `CreateSwapchainKHR`: record format, extent, image usage, present mode, and surface.
- `GetSwapchainImagesKHR`: store `VkImage` handles once Cemu enumerates them.
- `DestroySwapchainKHR`: clean up mirror resources for that swapchain.
- `QueuePresentKHR`: look up the presented swapchain and image index, then optionally insert the mirror pass.

In `CreateSwapchainKHR`, request `VK_IMAGE_USAGE_TRANSFER_DST_BIT` when the surface supports it. If the surface does not support transfer-dst, fall back to a color-attachment render pass if `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` is available. If neither is available, disable mirror mode with a one-shot warning.

Do not assume one swapchain forever. Cemu may recreate the swapchain on resize, fullscreen changes, or graphics setting changes.

## Synchronization Rules

The pre-present submission must preserve Cemu's WSI ordering:

- The mirror submit waits on every semaphore in the original `VkPresentInfoKHR`.
- The real present waits on a BetterVR-owned binary semaphore signaled by the mirror submit.
- Keep one command buffer, fence, and present semaphore per swapchain image so resources are not reused while an older present may still own them.
- Before reusing a per-image command buffer, wait for its mirror-submit fence and reset it.
- Preserve the original `VkPresentInfoKHR::pNext` chain, swapchain list, image indices, and result arrays.

For image layouts:

- Assume Cemu's swapchain image is in `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` at `QueuePresentKHR`.
- Transition presented image to `TRANSFER_DST_OPTIMAL` for the transfer-only MVP, or `COLOR_ATTACHMENT_OPTIMAL` for the fullscreen pass.
- Transition it back to `PRESENT_SRC_KHR` before calling the real present.

## Runtime Controls

Keep this off by default for beta stability.

Suggested environment variables:

- `BETTERVR_DESKTOP_MIRROR=1`: enable the default right-eye mirror.
- `BETTERVR_DESKTOP_MIRROR_EYE=right|left`: choose the monoscopic source eye.
- `BETTERVR_DESKTOP_MIRROR_MODE=mono|side-by-side|hud|debug`: select the mirror layout.
- `BETTERVR_DESKTOP_MIRROR_MAX_HZ=60`: optional throttle if the mirror pass ever becomes measurable.

Reasonable defaults for demo recording:

- enabled only when explicitly requested
- right-eye mono
- 16:9 aspect-preserving output
- HUD composited over 3D once the fullscreen pass exists
- no frame timing logs unless diagnostics are enabled

## Performance Budget

Target costs:

- Cemu thread CPU overhead: under 0.1ms at present time.
- GPU overhead for MVP blit: under 0.2ms on the existing target GPU.
- GPU overhead for composited fullscreen pass: under 0.5ms.
- No `vkQueueWaitIdle`.
- No CPU readback.
- No extra OpenXR work.
- No HMD-cadence mirror loop.

If mirror work exceeds budget, prefer throttling desktop mirror updates over touching VR timing.

## Implementation Phases

1. Add disabled-by-default mirror configuration and one-shot logs.
2. Track Cemu swapchain creation, images, and destruction.
3. Insert a no-op pre-present submission that preserves present synchronization.
4. Render a solid color test pattern into the Cemu swapchain image.
5. Add mirror-owned color/HUD textures and copy selected-eye content into them from existing capture hooks.
6. Implement transfer-only 3D mirror with aspect-preserving blit.
7. Add HUD copy path and HUD-only fallback.
8. Add fullscreen composite pipeline for 3D + alpha HUD.
9. Add mirror modes and small user-facing Linux setup notes for recording.
10. Add timing stats for mirror CPU submit time, GPU fence time, skipped frames, and swapchain recreation count.

## Test Checklist

- Mirror disabled: behavior and performance match the current stable Linux path.
- Mirror enabled before game launch: Cemu window shows a stable mirror once gameplay starts.
- OBS captures the Cemu window without capturing SteamVR's compositor window.
- Pause menu open/close does not perturb VR frame pairing or desktop mirror state.
- D-pad weapon menu and map screen remain stable.
- Resize or recreate Cemu window swapchain without leaking resources or crashing.
- Cemu exit tears down mirror resources before device destruction.
- If transfer-dst usage is unsupported, mirror disables itself with one warning and VR still works.

## Open Questions

- Whether the first demo needs HUD compositing or whether 3D-only mirror is acceptable for the initial recording.
- Whether the mirror should show the same eye crop as the headset or a wider 16:9 recording crop.
- Whether side-by-side output is useful for debugging, or whether mono right-eye should be the only beta-facing mode.

## Next Work Item

The current transfer-only mirror path is good enough for demo capture. The next rendering work item is a demo-ready composite path:

- Build a tiny fullscreen Vulkan pipeline for the Cemu swapchain.
- Sample the selected-eye 3D mirror texture and HUD mirror texture.
- Alpha-composite HUD over 3D in the desktop window.
- Keep the transfer-only mirror as the fallback if pipeline setup fails.
- Validate against gameplay, pause menu, D-pad weapon menu, map screen, OBS capture, and mirror-disabled VR performance before returning to Linux ImGui.
