# Linux Backend Refactor Plan

**Status:** In progress — Phase 0 (created 2026-05-28)
**Branch:** `linux-port`  ·  **Baseline:** merge-base with `main` (`db4a6f1`)

## 1. Motivation

The `linux-port` branch adds a native Linux **Vulkan + OpenXR** render path alongside the
upstream **Windows / D3D12** path (~5,100 insertions across 46 files). It works — the
re-present-at-native-vsync architecture is documented in `LINUX_PORT_STATUS.md` — but it is
intrusive:

- The single `RND_Renderer` class is **split across two `.cpp` files** (`renderer.cpp`, wrapped
  entirely in `#ifdef _WIN32`, and `renderer_linux.cpp`), sharing a header with `#ifdef`'d members.
- **228 added preprocessor-directive lines** interleave Linux logic into upstream files.
- Several changes **silently alter the Windows build** (unguarded behavior changes + portability
  edits to shared headers).

### Goals

1. All Linux renderer code lives in its own separate files.
2. The diff to upstream files is minimized — only the mandatory orchestration to select the
   backend, which stays **compile-time**.
3. Maintenance burden on upstream Windows devs is minimized: they should not have to read Linux
   concerns, and their code paths must not change behavior.

## 2. Review summary

### Already at the target (keep as-is)
- CMake `if(WIN32)/else()` source-list selection — the right mechanism.
- Whole-file `#ifdef _WIN32` wraps on `d3d12.h`, `d3d12_utils.h`, `swapchain.{h,cpp}`,
  `vulkan_imgui.cpp`, `shader.h`, and the D3D12 lower-half of `texture.{h,cpp}`.
- `linux_desktop_mirror.*` is a cleanly decoupled module.
- `instance.h` keeps every Windows path byte-identical (additive).

### Intrusive hotspots (ranked by maintenance cost)
| File | Problem |
|------|---------|
| `renderer.h` | Shared getters now take a `shared_lock` + call `LatchPendingIfNeeded()`; members became atomics; `Layer3D/Layer2D` `#ifdef`'d; Linux method decls inline. |
| `openxr.cpp` | 8×`#ifdef`/7×`#else` interleaved inside functions; `CreateSession` split mid-body; `ProcessEvents` gained shared mutex + session-state mirror + log changes. |
| `framebuffer.cpp` | +308 lines: Linux `#else` capture blocks, Linux-only diag statics, `IsLayerActiveForProcess()` injected into 6 shared overrides. |
| `vulkan.{h,cpp}` | **Unguarded** queue-topology globals/accessors (Linux-only, compiled into Windows); `IsLayerActiveForProcess()` early-out in a shared present-modes hook. |
| `layer.cpp` | New shared `IsLayerActiveForProcess()`, early-outs in 6 overrides, ~70-line Linux queue patch, `checkAssert→WARNING` downgrade. |
| `pch.h` | Platform headers cleanly guarded, but `BETypeCompatible` removal + `is_BEType_v` rework + `PADDED_BYTES` + extra includes are unguarded shared edits. |

### The reframe: three tangled changesets
1. **Linux backend** (the real goal) → separate files.
2. **Cross-compiler portability fixes** (Windows-affecting, mostly legit) → keep, but isolate.
3. **Unrelated behavior changes / debug noise** (Windows-affecting) → revert or guard.

## 3. Architecture decision: full backend seam

`RND_Renderer` becomes a thin **shared frontend** (what the hooks already talk to via
`VRManager::instance().XR->GetRenderer()`). It owns shared bookkeeping and holds a
`std::unique_ptr<RenderBackend>`. The backend owns everything that differs — including the
view/pose state and its threading — so the pose getters become uniform one-liners and **Windows
carries zero `shared_mutex`/latch overhead**.

```cpp
// rendering/render_backend.h  (shared)
class RenderBackend {
public:
  virtual ~RenderBackend() = default;
  virtual void OnSessionStarted(XrSession, XrSpace stage) = 0;
  virtual void BeginFrame() = 0;
  virtual void SubmitFrame() = 0;                 // Win: xrEndFrame here; Linux: hand to submit thread
  virtual bool HandleColorClear(/*…*/) = 0;       // Cemu clear-hook capture; returns "consumed?"
  virtual bool HandleDepthClear(/*…*/) = 0;
  virtual bool HandleHudClear(/*…*/) = 0;
  virtual void MarkInventoryPauseTransition(bool) {}
  virtual std::optional<std::array<XrView,2>> CurrentViews() = 0;   // Linux late-latch lives here
  virtual void UpdateViews(XrTime) = 0;
};
#ifdef _WIN32
  using ActiveBackend = D3D12Backend;
#else
  using ActiveBackend = VulkanBackend;
#endif
```

### Principles
1. **No Linux logic in upstream files** — only a narrow, well-named call into the backend.
2. **Windows is behavior-preserving by construction** — any change to what Windows compiles/runs
   is either a provably-equivalent portability fix or explicitly owned.
3. **One compile-time backend seam** — no scattered `#ifdef _WIN32` in logic bodies.
4. **Platform/compiler shims live in one compat header.**
5. **Portability fixes ship separately** from the Linux feature.

### Target file map
```
rendering/
  renderer.h / .cpp            shared frontend + unique_ptr<RenderBackend>
  render_backend.h             NEW: interface + compile-time ActiveBackend
  backends/
    renderer_d3d12.{h,cpp}     Windows  (was renderer.cpp + Layer3D/Layer2D)
    renderer_vulkan.{h,cpp}    Linux    (was renderer_linux.cpp)
    vulkan_queue_topology.{h,cpp}  Linux (was the globals in vulkan.{h,cpp})
  linux_desktop_mirror.*       Linux (already clean — keep)
platform/
  compat.h                     NEW: stricmp shims, PlatformModule (dlopen/LoadLibrary), timer typedef
  pch_win32.h / pch_posix.h    platform header blocks pulled out of pch.h
hooking/
  layer_active.{h,cpp}         NEW: IsLayerActiveForProcess() (cross-platform, out of layer.cpp)
```

### Irreducible upstream touch-points
| File | Minimal residual change |
|------|-------------------------|
| CMake | per-platform source-lists + defines (already done) |
| `pch.h` | two guarded includes (`pch_win32.h` / `pch_posix.h`) |
| `renderer.h` | frontend + `unique_ptr<RenderBackend>`; `Layer3D/Layer2D` + Linux decls removed |
| `openxr.{h,cpp}` | `CreateSession` takes a backend-supplied graphics binding |
| `instance.h` | calls `backend->CreateGraphicsBinding()` |
| `vulkan.h` | optional neutral `RND_Vulkan::GetQueue(role)` accessor |
| `framebuffer.cpp` | thin `renderer->Handle*Clear(...)` dispatch + shared `IsLayerActiveForProcess()` |

## 4. Triage of Windows-affecting changes (dispositions locked)

Legend: **KEEP** = legit, keep · **VERIFY** = keep after confirming equivalence · **GUARD** = make
Linux-only · **REVERT** = remove from branch.

### Group 1 — portability fixes (KEEP, isolated as an upstreamable group)
| # | Where | Change | Why needed | Disp. |
|---|-------|--------|-----------|-------|
| P1 | `game_structs.h` | `ActorFlags{,2,3}: int32_t→uint32_t` | `0x80000000` out-of-range for signed enum (GCC/Clang error) | KEEP |
| P2 | `pch.h` | `PADDED_BYTES` drop stray `##` | ill-formed token-paste | KEEP |
| P3 | `pch.h`+`game_structs.h` | drop `BETypeCompatible`, rework `is_BEType_v` | see §5 | **VERIFY→hardened** |
| P4 | `pch.h` | relocate `#pragma pack(push,1)` | pairs with P3 | KEEP |
| P5 | `pch.h` | add `<map>`,`<sstream>`,`glm/gtx/quaternion.hpp` | used by new code | KEEP |
| P6 | `profiler.h` | atomic NSDMI → ctor init-list | GCC/Clang aggregate+atomic init | KEEP |
| P7 | `logger.h` | `template<typename LogType L>`→`template<LogType L>` | old form malformed | KEEP |
| P8 | `logger.h` | extract `fatalError()` | DRY; equivalent | KEEP |
| P9 | `skeleton.cpp`,`weapon.cpp` | `getMemory<FixedSafeString*>`→`readMemory` | compensated P3 | **REVERT** (trait fixed) |
| P10 | `skeleton.cpp` | `cos/sin`→`cosf/sinf` | float overload | KEEP |
| P11 | `entity_debugger.cpp`,`imgui_menus.cpp` | `ImGui::Text("%s",…)` | `-Wformat-security` | KEEP |

### Group 2 — behavior / noise (mostly not "Linux port")
| # | Where | Change | Disp. |
|---|-------|--------|-------|
| B1 | `camera.cpp` | `kEyeForwardOffset=0.15f` | **GUARD (Linux-only)** |
| B2 | `camera.cpp` | `hook_GetRenderCamera` `+ \|\| UseMonoFrameBuffer…()` | **GUARD (Linux-only)** |
| B3 | `camera.cpp` | `hook_GetRenderProjection` debug logging | **REVERT** |
| B4 | `logger.cpp` | log `app→trunc` | **GUARD** (Win keeps `app`) |
| B5 | `logger.h` | removed `#ifndef _DEBUG` file-log guard | **GUARD** (Win keeps guard) |
| B6 | `openxr.cpp` | throw text dropped troubleshooting suffix | **REVERT** (restore) |
| B7 | `openxr.cpp` | `ProcessEvents` log levels VERBOSE→INFO/WARNING | KEEP (cosmetic) |
| B8 | `layer.cpp` | `checkAssert(Vulkan≥1.1)`→`WARNING` | **GUARD** (Win keeps assert) |
| C1 | `game_state.cpp` | `GetScreenDebugState` consolidation | KEEP (behavior-preserving) |

## 5. The `is_BEType_v` finding (P3 + P9)

`getMemory<T>` dispatches on `is_BEType_v<T>` (`true` ⇒ read raw; `false` ⇒ wrap in `BEType<T>` and
byte-swap). Upstream, `sead::SafeString : BETypeCompatible`, so the whole `SafeString` family
(`SafeString`→`BufferedSafeString`→`FixedSafeString40/100`) was `is_BEType_v == true`. The branch
dropped the base and added an explicit specialization list that **omits `SafeString`** — silently
flipping the family to `false`.

- **No live bug today:** no surviving `getMemory<T>` site uses a `SafeString`-derived type — the two
  that did (`skeleton.cpp`, `weapon.cpp`) were rewritten to `readMemory` (raw memcpy = the old
  `true`-branch). So P9 correctly compensated P3.
- **But a latent landmine:** any future `getMemory<sead::FixedSafeString40>(...)` (valid upstream)
  now compiles clean and silently returns garbage.

**Fix (Phase 0a):** replace the manual list with a non-layout-affecting member tag + concept —
```cpp
// on BEType<T>, SafeString, BEVec2/3, BEMatrix34/44:
using betype_tag = void;                         // a typedef adds ZERO layout (unlike a base class)
template<class T> concept BEReadable = requires { typename T::betype_tag; };
template<class T> inline constexpr bool is_BEType_v = BEReadable<T>;
```
This restores upstream semantics, removes the hand-maintained list, and avoids the empty-base /
`#pragma pack` interaction that motivated the original removal. With the trait restored,
`getMemory<FixedSafeString*>` works again, so **P9 reverts to the upstream form** (smaller diff).

## 6. Phased migration plan

Each phase ends with a Linux `nix` build + `steam-run` VR-present check. Windows is preserved by
construction.

| Phase | Scope | Exit criteria | Risk |
|------|-------|---------------|------|
| **0 — De-tangle** | **0a:** P3 hardened (tag-concept), P9 reverted, confirm `static_assert(sizeof…)`. **0b:** B1/B2/B4/B5/B8 → Linux-only, B3/B6 → revert. | Windows-affecting diff = only equivalent portability fixes. | Low |
| **1 — Compat layer** | `platform/compat.h` + `pch_{win32,posix}.h`; `PlatformModule` de-`#ifdef`s `cemu_hooks.h`+`settings.cpp`. | pch.h platform section = 2 includes; handle `#ifdef`s gone. | Low |
| **2 — Shared infra** | `IsLayerActiveForProcess()`→`hooking/layer_active.*`; guard/relocate `ProcessEvents` mutex + session-state mirror. | `layer.cpp`/`openxr.cpp` shared bodies ≈ upstream. | Low–Med |
| **3 — Backend seam** | `render_backend.h`; `renderer.cpp`→`D3D12Backend`, `renderer_linux.cpp`→`VulkanBackend`; thin `RND_Renderer`; move view/latch/mutex into backend; add `OpenXR::GetStageSpace()`. | one class per backend file; uniform getters; `poseAge` unchanged. | **Med–High** |
| **4 — Hook seam** | `framebuffer.cpp` clear hooks → `renderer->Handle*Clear(…)`; Linux diag statics → `VulkanBackend`. | framebuffer.cpp diff ≈ dispatch + shared guard. | Med |
| **5 — Vulkan plumbing** | queue-topology globals → `backends/vulkan_queue_topology.*`; mirror swapchain overrides routed only when Vulkan backend active. | vulkan.{h,cpp} diff ≈ a neutral accessor. | Low–Med |
| **6 — Session binding** | `backend->CreateGraphicsBinding()` feeds `OpenXR::CreateSession`; collapse `#ifdef/#else` in openxr.cpp + instance.h. | no graphics-API `#ifdef` in `CreateSession`. | Med |

Phases 0–2 capture nearly all of the "minimize upstream maintenance burden" win at low risk; Phase 3
is the principled core. The refactor is **relocation, not a threading rewrite** — the working
submit/pose-thread logic moves verbatim.

## 7. Verification

- **Linux:** `nix develop --command bash -c "cmake --preset Linux-Debug && cmake --build cmake-build-Linux-Debug"`,
  then `steam-run ./start-steamvr.sh` + `steam-run ./launch-bettervr.sh`; confirm VR presents and
  `poseAge` p50/p99 are unchanged across Phase 3.
- **Windows:** cannot be built in this environment; preserved by construction (Phase 0 removes all
  Windows-affecting edits; later phases only relocate Windows code unchanged into `D3D12Backend`).
  A Windows dev should compile-check after Phase 3.
