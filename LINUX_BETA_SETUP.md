# BetterVR Linux Beta Setup

This is the native Linux Vulkan/OpenXR beta path for BetterVR. It is intended for testers who already have Breath of the Wild working in Cemu before enabling BetterVR.

## Requirements

- A legal Wii U copy of Breath of the Wild with the latest update installed in Cemu.
- Cemu 2.6 or newer, using the Vulkan renderer.
- A working SteamVR setup on Linux.
- Current GPU drivers with Vulkan support.
- Nix with `nix-command` and `flakes` enabled.
- The BetterVR graphic pack enabled in Cemu, plus FPS++ enabled.
- The repo-local patched Cemu build from `nix build .#cemu -o result-cemu`. BetterVR needs Cemu to export hook symbols that stock Cemu builds usually do not export.

## Steam Launch Rule

Steam grabs controller focus for applications it owns. Because of that, do not start SteamVR inside Steam and then start Cemu outside Steam.

Use one of these two patterns:

- Start both outside Steam: run `./launch-bettervr.sh` from a terminal and let it start SteamVR if needed.
- Start both inside Steam: add the BetterVR launcher command as a non-Steam game and launch it from Steam.

Mixing those modes can leave Cemu unable to see the controllers correctly.

## Build From This Repo

First-run sequence from the repo root:

```sh
nix develop
cmake --preset Linux-Release
cmake --build cmake-build-Linux-Release
exit
nix build .#cemu -o result-cemu
```

The same BetterVR layer build can also be run without staying inside the shell:

```sh
nix develop --command bash -c 'cmake --preset Linux-Release && cmake --build cmake-build-Linux-Release'
```

Build the repo-local patched Cemu package. This command can be run either inside or outside `nix develop`:

```sh
nix build .#cemu -o result-cemu
```

If `steamvr` or `vrstartup.sh` is on `PATH`, the launcher can find SteamVR automatically. Cemu is intentionally not selected from `PATH`; the default is the repo-local `./result-cemu/bin/Cemu` build so the launcher does not accidentally pick a stock Cemu without BetterVR hook exports.

## Configure Cemu

First confirm the game works normally in Cemu without BetterVR.

Recommended Cemu settings:

- `Options` -> `General Settings` -> `Graphics`: Renderer `Vulkan`.
- Make sure SteamVR is selected as the active OpenXR runtime in SteamVR's settings.
- Disable VSync.
- `Debug` -> `Accurate Barriers (Vulkan)`: disabled.
- Download current community graphic packs.
- Enable `The Legend of Zelda: Breath of the Wild` -> `Mods` -> `FPS++`.
- Enable `The Legend of Zelda: Breath of the Wild` -> `Mods` -> `BetterVR`.
- Set the FPS++ limit to at least 120 or 144.

The launcher can symlink the BetterVR graphic pack automatically when it can find Cemu's graphic pack folder. If it cannot, set one of:

```sh
export CEMU_GRAPHIC_PACKS="$HOME/.local/share/Cemu/graphicPacks"
# or
export CEMU_DIR="/path/to/your/Cemu directory"
```

## Launch

Set the path to BotW's RPX file:

```sh
export BOTW_RPX="/path/to/The Legend of Zelda Breath of the Wild/code/U-King.rpx"
```

The launcher uses `./result-cemu/bin/Cemu` by default. To force a different compatible Cemu build:

```sh
export CEMU_BIN="$PWD/result-cemu/bin/Cemu"
```

Then run:

```sh
./launch-bettervr.sh
```

Do not wrap the launcher in `steam-run` for the outside-Steam path. Run `./launch-bettervr.sh` directly; it starts SteamVR before exporting the BetterVR Vulkan layer variables and then launches Cemu in the same ownership mode.

If `steamvr` or `vrstartup.sh` is on `PATH`, `result-cemu` has been built, and the BetterVR layer has been built in the repo, this should work with no additional path configuration beyond `BOTW_RPX`.

The launcher behavior is:

- If SteamVR is already running, it uses that instance and leaves it running after Cemu exits.
- If SteamVR is not running, it starts SteamVR, waits for `vrserver`, launches Cemu, and shuts down that SteamVR instance when Cemu exits.
- If `BOTW_RPX` is set, it passes `-g "$BOTW_RPX"` to Cemu. If it is unset, Cemu opens normally.
- It enables the BetterVR Vulkan layer only for the Cemu launch; SteamVR is started before those Vulkan layer variables are exported.

## Launcher Defaults And Overrides

The launcher tries these defaults before requiring manual paths:

- SteamVR: `STEAMVR_CMD`, then `steamvr` on `PATH`, then `vrstartup.sh` on `PATH`, then common Steam library locations.
- Cemu: `CEMU_BIN` if set, otherwise `./result-cemu/bin/Cemu`. The launcher checks that the selected Cemu exports the BetterVR hook symbols before launching.
- BetterVR layer: `BETTERVR_LAYER_DIR`, then `cmake-build-Linux-Release/lib`, `cmake-build-Linux-RelWithDebInfo/lib`, `cmake-build-Linux-Debug/lib`, and common Nix result symlinks.
- Graphic packs: `CEMU_GRAPHIC_PACKS`, then `CEMU_DIR/graphicPacks`, then common Cemu user directories, then a `graphicPacks` directory next to the Cemu executable.

Useful overrides:

```sh
export STEAMVR_CMD="steamvr"
export STEAMVR_DIR="$HOME/.local/share/Steam/steamapps/common/SteamVR"
export STEAM_ROOT="$HOME/.local/share/Steam"
export STEAM_RUNTIME_RUN="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper/run"
export BETTERVR_LAYER_DIR="$PWD/cmake-build-Linux-Release/lib"
```

## Diagnostics

`BetterVR_log.txt` is rewritten on each run.

For optimization runs:

```sh
BETTERVR_LINUX_FRAME_STATS=1 ./launch-bettervr.sh
```

For desktop recording, enable the optional Cemu-window mirror before launching:

```sh
BETTERVR_DESKTOP_MIRROR=1 ./launch-bettervr.sh
```

The mirror is off by default. When enabled, it draws the right-eye gameplay capture into Cemu's normal desktop window so OBS can capture Cemu instead of SteamVR. To record the left eye instead:

```sh
BETTERVR_DESKTOP_MIRROR=1 BETTERVR_DESKTOP_MIRROR_EYE=left ./launch-bettervr.sh
```

For noisy capture/menu diagnostics:

```sh
BETTERVR_LINUX_CAPTURE_DIAG=1 ./launch-bettervr.sh
```

## Troubleshooting

- If Cemu opens but BetterVR does not activate, confirm the BetterVR graphic pack is enabled and the launcher found a layer directory containing `BetterVR_Layer_linux.json` or `BetterVR_Layer.json`.
- If the launcher cannot find Cemu, run `nix build .#cemu -o result-cemu` from the repo root.
- If the log says Cemu hook symbols are missing, the launcher was bypassed or the symbol check was disabled. Use the repo's patched Cemu package or another BetterVR-compatible Cemu build.
- If SteamVR controllers do not reach Cemu, check the Steam launch rule above: SteamVR and Cemu should both be inside Steam, or both outside Steam.
- If the game is green, black, or flat, confirm Cemu is using Vulkan and the BetterVR/FPS++ graphic packs are enabled.
