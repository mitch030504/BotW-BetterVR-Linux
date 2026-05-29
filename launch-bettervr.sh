#!/usr/bin/env bash
# Launch Cemu with BetterVR on Linux. Starts SteamVR when needed and only shuts
# down the SteamVR instance it started itself.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
STEAMVR_STARTED_BY_SCRIPT=0
STEAMVR_PID=""

usage() {
    cat <<'EOF'
Usage:
  ./launch-bettervr.sh [--] [extra Cemu args...]

Common environment overrides:
  BOTW_RPX             Path to BotW's code/U-King.rpx. If unset, Cemu opens normally.
  CEMU_BIN             Explicit Cemu executable override. Defaults to the repo-local
                       ./result-cemu/bin/Cemu built by `nix build .#cemu -o result-cemu`.
  BETTERVR_LAYER_DIR   Directory containing BetterVR_Layer_linux.json or BetterVR_Layer.json.
                       Defaults to ./cmake-build-Linux-Release/lib, then RelWithDebInfo,
                       Debug, then common Nix package result symlinks.
  CEMU_DIR             Cemu user/install directory where graphicPacks should be linked.
  CEMU_GRAPHIC_PACKS   Explicit Cemu graphicPacks directory.

SteamVR overrides:
  STEAMVR_CMD          Command used to start SteamVR, for example "steamvr".
  STEAMVR_DIR          SteamVR install directory containing bin/vrstartup.sh.
  STEAM_ROOT           Steam install root. Defaults to common Steam locations.
  STEAM_RUNTIME_RUN    Steam Linux Runtime runner. Defaults to common sniper paths.
  BETTERVR_SKIP_CEMU_SYMBOL_CHECK
                       Set to 1 to bypass the Cemu hook-symbol preflight.

Both Cemu and SteamVR may be launched inside Steam, or both may be launched
outside Steam. Do not launch SteamVR inside Steam while launching Cemu outside
Steam: Steam will grab controller focus from the external Cemu process.
EOF
}

log() {
    printf '[BetterVR] %s\n' "$*" >&2
}

die() {
    echo "[BetterVR] $*" >&2
    exit 1
}

prepend_env_path() {
    local var_name="$1"
    local entry="$2"
    local old_value="${!var_name:-}"
    if [[ -n "$old_value" ]]; then
        export "$var_name=$entry:$old_value"
    else
        export "$var_name=$entry"
    fi
}

is_truthy() {
    case "${1:-}" in
        ""|0|false|FALSE|no|NO) return 1 ;;
        *) return 0 ;;
    esac
}

steamvr_is_running() {
    pgrep -x vrserver >/dev/null 2>&1 || pgrep -f '/vrserver([[:space:]]|$)' >/dev/null 2>&1
}

common_steam_roots() {
    if [[ -n "${STEAM_ROOT:-}" ]]; then printf '%s\n' "$STEAM_ROOT"; fi
    if [[ -n "${STEAM_BASE_FOLDER:-}" ]]; then printf '%s\n' "$STEAM_BASE_FOLDER"; fi
    printf '%s\n' \
        "$HOME/.local/share/Steam" \
        "$HOME/.steam/steam" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
}

steam_library_roots() {
    local root vdf
    while IFS= read -r root; do
        [[ -d "$root" ]] || continue
        printf '%s\n' "$root"
        vdf="$root/steamapps/libraryfolders.vdf"
        if [[ -f "$vdf" ]]; then
            awk -F '"' '/"path"[[:space:]]+"/ { print $4 }' "$vdf"
        fi
    done < <(common_steam_roots)
}

find_steam_common_file() {
    local rel="$1"
    local root candidate
    while IFS= read -r root; do
        [[ -n "$root" ]] || continue
        candidate="$root/steamapps/common/$rel"
        if [[ -e "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done < <(steam_library_roots | awk '!seen[$0]++')
    return 1
}

find_steamvr_command() {
    local startup_script runtime_run
    STEAMVR_COMMAND=()

    if [[ -n "${STEAMVR_CMD:-}" ]]; then
        STEAMVR_COMMAND=(bash -lc "exec $STEAMVR_CMD")
    elif command -v steamvr >/dev/null 2>&1; then
        STEAMVR_COMMAND=("$(command -v steamvr)")
    elif command -v vrstartup.sh >/dev/null 2>&1; then
        STEAMVR_COMMAND=("$(command -v vrstartup.sh)")
    else
        if [[ -n "${STEAMVR_DIR:-}" && -x "$STEAMVR_DIR/bin/vrstartup.sh" ]]; then
            startup_script="$STEAMVR_DIR/bin/vrstartup.sh"
        else
            startup_script="$(find_steam_common_file "SteamVR/bin/vrstartup.sh" || true)"
        fi

        [[ -n "$startup_script" ]] || return 1

        if [[ -n "${STEAM_RUNTIME_RUN:-}" && -x "$STEAM_RUNTIME_RUN" ]]; then
            STEAMVR_COMMAND=("$STEAM_RUNTIME_RUN" -- "$startup_script")
        else
            runtime_run="$(find_steam_common_file "SteamLinuxRuntime_sniper/run" || true)"
            if [[ -n "$runtime_run" && -x "$runtime_run" ]]; then
                STEAMVR_COMMAND=("$runtime_run" -- "$startup_script")
            else
                STEAMVR_COMMAND=("$startup_script")
            fi
        fi
    fi
}

start_steamvr_if_needed() {
    if steamvr_is_running; then
        log "Using already-running SteamVR."
        return 0
    fi

    find_steamvr_command || die "Could not find SteamVR. Put steamvr or vrstartup.sh on PATH, or set STEAMVR_CMD/STEAMVR_DIR."

    local steam_root
    steam_root="$(common_steam_roots | while IFS= read -r root; do [[ -d "$root" ]] && { printf '%s\n' "$root"; break; }; done)"
    if [[ -n "$steam_root" ]]; then
        export STEAM_BASE_FOLDER="${STEAM_BASE_FOLDER:-$steam_root}"
    fi

    log "Starting SteamVR: ${STEAMVR_COMMAND[*]}"
    "${STEAMVR_COMMAND[@]}" &
    STEAMVR_PID=$!
    STEAMVR_STARTED_BY_SCRIPT=1

    log "Waiting for SteamVR..."
    for _ in $(seq 1 90); do
        if steamvr_is_running; then
            log "SteamVR is running."
            return 0
        fi
        sleep 1
    done

    die "SteamVR did not report a running vrserver."
}

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM

    if [[ "$STEAMVR_STARTED_BY_SCRIPT" -eq 1 ]]; then
        log "Stopping SteamVR started by this script."
        pkill -TERM -x vrmonitor 2>/dev/null || true
        pkill -TERM -x vrcompositor 2>/dev/null || true
        pkill -TERM -x vrserver 2>/dev/null || true
        if [[ -n "$STEAMVR_PID" ]]; then
            wait "$STEAMVR_PID" 2>/dev/null || true
        fi
    fi

    exit "$exit_code"
}

find_cemu_bin() {
    if [[ -n "${CEMU_BIN:-}" ]]; then
        [[ -x "$CEMU_BIN" ]] || die "CEMU_BIN is not executable: $CEMU_BIN"
        if ! cemu_is_bettervr_compatible "$CEMU_BIN"; then
            die "CEMU_BIN does not export the BetterVR hook symbols: $CEMU_BIN"
        fi
        printf '%s\n' "$CEMU_BIN"
        return 0
    fi

    local local_cemu="$SCRIPT_DIR/result-cemu/bin/Cemu"
    [[ -x "$local_cemu" ]] || die "Could not find repo-local patched Cemu at $local_cemu. Run: nix build .#cemu -o result-cemu"
    if ! cemu_is_bettervr_compatible "$local_cemu"; then
        die "Repo-local Cemu does not export the BetterVR hook symbols: $local_cemu"
    fi
    printf '%s\n' "$local_cemu"
}

cemu_is_bettervr_compatible() {
    local binary="$1"
    [[ -x "$binary" ]] || return 1

    if is_truthy "${BETTERVR_SKIP_CEMU_SYMBOL_CHECK:-}"; then
        return 0
    fi

    local target
    while IFS= read -r target; do
        [[ -n "$target" && -x "$target" ]] || continue
        if cemu_binary_exports_hooks "$target"; then
            return 0
        fi
    done < <(cemu_symbol_check_targets "$binary")

    return 1
}

cemu_symbol_check_targets() {
    local binary="$1"
    local resolved dir base
    {
        printf '%s\n' "$binary"
        resolved="$(readlink -f "$binary" 2>/dev/null || true)"
        if [[ -n "$resolved" ]]; then
            printf '%s\n' "$resolved"
        fi
    } | while IFS= read -r candidate; do
        [[ -n "$candidate" ]] || continue
        printf '%s\n' "$candidate"
        dir="$(cd -- "$(dirname -- "$candidate")" 2>/dev/null && pwd -P || true)"
        base="$(basename -- "$candidate")"
        if [[ -n "$dir" ]]; then
            printf '%s\n' "$dir/.$base-wrapped"
        fi
    done | awk 'NF && !seen[$0]++'
}

cemu_binary_exports_hooks() {
    local binary="$1"
    local symbols=""
    if command -v nm >/dev/null 2>&1; then
        symbols="$(nm -D "$binary" 2>/dev/null || true)"
    elif command -v readelf >/dev/null 2>&1; then
        symbols="$(readelf -Ws "$binary" 2>/dev/null || true)"
    elif command -v objdump >/dev/null 2>&1; then
        symbols="$(objdump -T "$binary" 2>/dev/null || true)"
    else
        die "Cannot check Cemu hook symbols because nm/readelf/objdump are unavailable. Install binutils or set BETTERVR_SKIP_CEMU_SYMBOL_CHECK=1."
    fi

    [[ "$symbols" == *gameMeta_getTitleId* ]] &&
    [[ "$symbols" == *memory_getBase* ]] &&
    [[ "$symbols" == *osLib_registerHLEFunction* ]]
}

find_layer_dir() {
    local candidate
    if [[ -n "${BETTERVR_LAYER_DIR:-}" ]]; then
        candidate="$BETTERVR_LAYER_DIR"
        [[ -f "$candidate/BetterVR_Layer_linux.json" || -f "$candidate/BetterVR_Layer.json" ]] || die "BETTERVR_LAYER_DIR is missing BetterVR_Layer_linux.json or BetterVR_Layer.json: $candidate"
        printf '%s\n' "$candidate"
        return 0
    fi

    for candidate in \
        "$SCRIPT_DIR/cmake-build-Linux-Release/lib" \
        "$SCRIPT_DIR/cmake-build-Linux-RelWithDebInfo/lib" \
        "$SCRIPT_DIR/cmake-build-Linux-Debug/lib" \
        "$SCRIPT_DIR/result-bettervr/share/vulkan/implicit_layer.d" \
        "$SCRIPT_DIR/result/share/vulkan/implicit_layer.d"
    do
        if [[ -f "$candidate/BetterVR_Layer_linux.json" || -f "$candidate/BetterVR_Layer.json" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    die "Could not find the BetterVR Vulkan layer. Build first or set BETTERVR_LAYER_DIR."
}

install_graphic_pack_link() {
    local cemu_bin="$1"
    local source_pack="$SCRIPT_DIR/resources/BreathOfTheWild_BetterVR"
    local target_root=""
    local cemu_bin_dir

    [[ -d "$source_pack" ]] || return 0

    if [[ -n "${CEMU_GRAPHIC_PACKS:-}" ]]; then
        target_root="$CEMU_GRAPHIC_PACKS"
    elif [[ -n "${CEMU_DIR:-}" ]]; then
        target_root="$CEMU_DIR/graphicPacks"
    elif [[ -d "$HOME/.local/share/Cemu/graphicPacks" ]]; then
        target_root="$HOME/.local/share/Cemu/graphicPacks"
    elif [[ -d "$HOME/.config/Cemu/graphicPacks" ]]; then
        target_root="$HOME/.config/Cemu/graphicPacks"
    else
        cemu_bin_dir="$(cd -- "$(dirname -- "$cemu_bin")" && pwd -P)"
        if [[ -d "$cemu_bin_dir/graphicPacks" ]]; then
            target_root="$cemu_bin_dir/graphicPacks"
        else
            return 0
        fi
    fi

    mkdir -p "$target_root"
    ln -sfn "$source_pack" "$target_root/BreathOfTheWild_BetterVR"
    log "Linked BetterVR graphic pack into $target_root."
}

declare -a CEMU_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            CEMU_ARGS+=("$@")
            break
            ;;
        *)
            CEMU_ARGS+=("$1")
            shift
            ;;
    esac
done

trap cleanup EXIT INT TERM

start_steamvr_if_needed

cemu_bin="$(find_cemu_bin)"
layer_dir="$(find_layer_dir)"
install_graphic_pack_link "$cemu_bin"

prepend_env_path VK_ADD_LAYER_PATH "$layer_dir"
prepend_env_path VK_LAYER_PATH "$layer_dir"
export VK_INSTANCE_LAYERS=VK_LAYER_CREMENTIF_bettervr
export ENABLE_BETTERVR_MOD=1

if [[ -n "${BOTW_RPX:-}" ]]; then
    [[ -f "$BOTW_RPX" ]] || die "BOTW_RPX does not exist: $BOTW_RPX"
    CEMU_ARGS=(-g "$BOTW_RPX" "${CEMU_ARGS[@]}")
fi

log "Launching Cemu: $cemu_bin ${CEMU_ARGS[*]}"
log "Using BetterVR Vulkan layer from $layer_dir"
set +e
"$cemu_bin" "${CEMU_ARGS[@]}"
cemu_status=$?
set -e
log "Cemu exited with status $cemu_status."
exit "$cemu_status"
