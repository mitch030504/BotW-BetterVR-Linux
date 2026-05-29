# BetterVR Linux Port Status

## Current State: Linux beta prep after pause-menu recovery (2026-05-27)

Stereo 3D VR gameplay with head tracking, HUD, depth composition, menu support,
and **pose-driven re-presentation at native vsync rate**. SteamVR's motion
smoothing is replaced by our own depth-aware re-presentation path. The previous
SteamVR/vrcompositor crash appears improved after the layer process guard, but
remains a watch item.

The main pause-menu open/close path no longer causes the persistent post-pause
performance drop. Pause transitions are diagnostic-only; the Linux 3D capture
path self-synchronizes from the actual left/right clear sequence. Startup docs
now use a single `launch-bettervr.sh` entrypoint that starts SteamVR only when
needed and shuts down only the SteamVR instance it owns.

## Architecture (working as designed)

```
Cemu thread (~45 Hz, game-paced):
  - Renders game; our hooks blit to intermediate[*][currentWriteIdx_3D/hud]
  - QueuePresentKHR fires:
    - Signal-only submission on queue 0 → timeline semaphore++
    - Publish latest content state (per-layer readBufferIdx)
    - Toggle currentWriteIdx for any layer that was written

Submit thread (75/90 Hz, native vsync paced via xrWaitFrame):
  - Snapshot latest content state
  - If contentSeq is new, acquire OpenXR swapchain images and record copies:
    intermediate[*][latest.readBufferIdx_*] → swapchain
  - If contentSeq is unchanged, reuse already-copied swapchain images
  - Build composition layers:
    - 3D projection uses the saved content render pose/FOV
    - HUD quad uses the freshest late-located pose
  - xrEndFrame (runtime submits compositor work on queue 1)
  - xrWaitFrame (paces 75/90 Hz)
  - xrLocateViews + UpdateViews + UpdateActions

Pose thread (~250 Hz):
  - Continuously samples xrLocateViews
  - Stores latest in poseTracking.latestRawViews
  - Submit thread can use this as a fallback when late-locate fails
```

## Achieved performance characteristics

Recent local test runs:
- **Cemu render**: ~16-22ms (game-bound, unchanged)
- **Submit thread iteration**: native vsync paced (~13.3ms at 75 Hz, ~16.7ms at 60 Hz)
- **`startFrame` on Cemu thread**: ~0.01ms (no longer waiting for OpenXR lifecycle)
- **`endFrame` on Cemu thread**: ~0.01ms (no longer doing GPU copies)
- **`poseAge`**: ~0ms p50/p99 in current late-locate path
- **`contentAge`**: commonly ~10-20ms during gameplay, higher when Cemu drops below native HMD rate
- **`xrEndFrame`**: 4-6ms p50 on submit thread
- **Swapchain copies**: now only happen for new content; reused submit frames re-submit layers without recopying
- **Depth reprojection input**: Linux now crops depth with the same per-eye sub-FOV as color before submitting depth composition
- **3D color target selection**: Linux now follows the Windows rule and accepts only the `A2B10G10R10_UNORM` display target for 3D color capture

## Working features

- [x] Build system (nix flake, CMake cross-platform)
- [x] Vulkan layer loads and hooks Cemu
- [x] Patched Cemu exports hook symbols (`nix build .#cemu`)
- [x] OpenXR session with SteamVR (Vulkan binding)
- [x] Stereo 3D rendering (both eyes, correct projection/FOV)
- [x] Per-eye UV transform (FOV cropping from 16:9 render)
- [x] Depth buffer capture and composition
- [x] Head tracking with seated height offset
- [x] In-game HUD with transparency
- [x] Pause menu, inventory, weapon select menus
- [x] Title screen / save select as floating panel
- [x] Basic fade effects
- [x] **Late-latched head pose via dedicated thread** (poseAge 32× improvement)
- [x] **Async submit thread** owns the OpenXR frame lifecycle
- [x] **Re-present at native vsync** — submit thread reuses copied swapchain images and submits fresh composition layers every 13-17ms
- [x] **Projection layer render-pose tracking** — stale 3D content keeps the pose/FOV it was rendered from so the runtime can reproject it
- [x] **Depth/color registration on Linux** — color and depth intermediates use the same per-eye UV crop before projection-layer submission
- [x] **Linux 3D color target filter** — non-display `captureIdx == 0` targets are skipped so they cannot overwrite per-eye 3D intermediates
- [x] **Double-buffered intermediates** with per-layer write tracking
- [x] **Three-queue topology**: queue 0 (Cemu), queue 1 (OpenXR runtime), queue 2 (our copies)
- [x] Opt-in frame timing instrumentation with periodic stats
- [x] Submit thread step markers for stall diagnosis
- [x] Clean exit on VK_ERROR_DEVICE_LOST or fence timeout

## Outstanding Issues

### SteamVR/vrcompositor crash after sustained gameplay

SteamVR's vrcompositor `Render` thread has repeatedly hung inside
`CGpuTiming::GetDeltas` -> `libnvidia-glcore.so` after sustained xrEndFrame
submissions. After about 6 seconds the SteamVR watchdog aborts.

**Confirmed from coredump stack traces** (Tue 2026-05-26 09:48 and 10:14):
```
Thread "Render":
  libnvidia-glcore.so + 0xd34514                ← NVIDIA driver
  CGpuTiming::GetDeltas
  CGraphicsDevice::WaitForPresent
  CRenderThread::Update
  CRenderThread::Run
```

Other vrcompositor threads simultaneously hung in `libnvidia-glcore.so` on
futex waits — entire NVIDIA driver state is unresponsive.

However, a separate vrcompositor SIGSEGV coredump from Tue 2026-05-26 10:08
showed `libBetterVR_Layer.so` loaded in vrcompositor, with the render thread
inside BetterVR's Vulkan layer `QueuePresentKHR -> OpenXR::ProcessEvents`.
That means the layer was not reliably isolated to Cemu. A process guard now
makes every Vulkan hook pass through unless the process is Cemu or exports the
patched Cemu hook symbols.

Current diagnosis: the NVIDIA stack trace is real, but the crash could not be
called upstream-only until a fresh run confirmed vrcompositor no longer loaded
or executed BetterVR hook logic. The first post-guard gameplay retest ran about
7 minutes without instability, so this is no longer the active blocker but
should stay on the watch list.

Hypotheses tried before the process-guard finding:
- Pose thread rate (was 1000Hz, reduced to 250Hz — no change)
- Queue topology (shared → separate runtime + copy queues — no change in crash time)
- Double-buffered intermediates — fixed cross-queue races but not the hang

**Next verification if it returns**: reproduce and then check
`coredumpctl info <vrcompositor-pid>` or `BetterVR_log.txt`. A remaining
vrcompositor coredump should not have a `libBetterVR_Layer.so` frame. If it
still hangs in `CGpuTiming::GetDeltas` with no BetterVR frames, the next target
is driver/runtime reporting.

**Driver involved**: NVIDIA 590.48.01 / Linux kernel 6.12.63. Investigation
should look at:
- NVIDIA driver bug tracker for known Vulkan timing-query issues
- Possibly try a newer driver
- Mozilla crash IDs from `/tmp/dumps/` for upstream report

### Other known issues (BetterVR-side)

- Small perspective/FOV distortion may remain on some headsets and needs broader headset coverage.
- Link's internal body geometry visible in first-person (needs visibility hook tuning)
- Linux beta currently expects patched Cemu hook-symbol exports. The launcher defaults to the repo-local `result-cemu` symlink produced by `nix build .#cemu -o result-cemu`.
- Steam controller ownership matters: launch both SteamVR and Cemu inside Steam, or launch both outside Steam through `launch-bettervr.sh`.

## Key Files

- `src/rendering/renderer_linux.cpp` — Submit thread, EndFrame, intermediates, pose thread
- `src/rendering/renderer.h` — RND_Renderer class, GetPoses, m_currViews
- `src/hooking/framebuffer.cpp` — Frame capture (CmdClearColorImage hooks)
- `src/hooking/camera.cpp` — VR camera positioning, eye offset
- `src/rendering/openxr.cpp` — OpenXR session, session state tracking
- `src/rendering/vulkan.h`/`vulkan.cpp` — Queue index management
- `src/instance.h` — VRManager initialization (applies queue indices)
- `src/hooking/layer.cpp` — CreateDevice modified to request 2 extra queues
- `flake.nix` — Nix build, patched Cemu overlay

## Build & Run

```sh
# Build BetterVR layer
nix develop --command bash -c "cmake --preset Linux-Debug && cmake --build cmake-build-Linux-Debug"

# Build repo-local patched Cemu (first time only)
nix build .#cemu -o result-cemu

# Launch
BOTW_RPX="/path/to/BotW/code/U-King.rpx" ./launch-bettervr.sh

# Optional timing logs for optimization runs
BETTERVR_LINUX_FRAME_STATS=1 BOTW_RPX="/path/to/BotW/code/U-King.rpx" ./launch-bettervr.sh
```

Run the launcher directly for the outside-Steam path. Do not wrap it in
`steam-run`; the script handles SteamVR startup and Cemu environment setup.

For end-user setup details, see `LINUX_BETA_SETUP.md`.

## Key Technical Decisions

- **Re-present at native vsync (Phase 7)**: submit thread reuses copied swapchain images and submits composition layers at each headset vsync, replacing SteamVR's motion smoothing with our own depth-aware reprojection.
- **Projection render pose for 3D**: projection-layer `pose`/`fov` must describe the view that produced the submitted 3D image, not the current HMD pose. Fresh HMD pose is used for HUD quads and as a fallback only.
- **Depth follows color crop**: Linux color and depth copies both crop the 16:9 game render to the same per-eye OpenXR sub-FOV. Submitting full-frame depth with cropped color can destabilize depth-based reprojection.
- **3D color target filtering**: BotW emits multiple `captureIdx == 0` clears. Only the `VK_FORMAT_A2B10G10R10_UNORM_PACK32` target is the display 3D color target; `VK_FORMAT_B10G11R11_UFLOAT_PACK32` and other non-display targets must not overwrite the VR intermediate.
- **Three-queue topology**: queue 0 (Cemu), queue 1 (OpenXR runtime composition), queue 2 (our submit-thread copies). Each isolated to prevent saturation.
- **Per-layer double-buffered intermediates**: separate write indices for 3D and HUD prevent reading from buffers Cemu never wrote (was a source of VK_ERROR_DEVICE_LOST).
- **Timeline semaphore for queue 0 → queue 2 sync**: Cemu's queue 0 signals after intermediate writes, submit thread's queue 2 waits before reading.
- **Sticky content flags**: once Cemu shows a layer, it stays "present" even on frames where Cemu doesn't refresh — submit thread keeps re-presenting the last valid intermediate.
- **Late-latched head pose**: submit thread late-locates immediately before layer construction; the pose thread at ~250 Hz remains a fallback source. HUD uses the freshest pose for minimal motion-to-photon latency.
- **OpenXR init deferred**: created at first VkCreateDevice rather than at library load, avoiding deadlock with SteamVR's Vulkan loader callbacks.
- **Cemu patched at the nix level**: `DLLEXPORT` set to `__attribute__((visibility("default")))` + `--export-dynamic` linker flag, so we can `dlsym` the hooks.
