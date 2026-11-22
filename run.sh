#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# BB_PREBUILT=1 (packaged builds, the AppImage): out/bb-probe and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# BB_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default this directory.
data=${BB_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export BB_CONFIG=${BB_CONFIG:-$data/bbport.ini}
# GPU paths are initialized when libbbgpu is loaded, before bbgpu_init receives --user.
export BB_GPU_USER_DIR=${BB_GPU_USER_DIR:-${BB_USER_DIR:-$data/user}}
export __GL_SHADER_DISK_CACHE=${__GL_SHADER_DISK_CACHE:-1}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
mkdir -p "$BB_GPU_USER_DIR"
# FSR 4.1.1 assets (tools/fsr4cap/build_assets.sh): next to run.sh or in the data directory.
if [[ -z ${BB_FSR411_DIR:-} && ! -d fsr4_411 && -d $data/fsr4_411 ]]; then
    export BB_FSR411_DIR=$data/fsr4_411
fi
if [[ -z ${BB_PREBUILT:-} && -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
# BB_SAVE_LOG=1 (launcher: "Save the log and statistics to a file"): this run's output and its
# per-frame statistics also go to $data/logs/<time>.log, .frames.csv and .readbacks.csv.
if [[ ${BB_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    mkdir -p "$logs"
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
    export BB_FRAME_STATS=1 BB_FRAME_LOG=${BB_FRAME_LOG:-$logs/$stamp.frames.csv}
    export BB_READBACK_LOG=${BB_READBACK_LOG:-$logs/$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    # A descriptor bash picks (10 and up), not 3 (preserves AppImage runtime pipe).
    exec {log_fd}>"$logs/$stamp.log"
    exec > >(while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; printf '%s\n' "$line" >&$log_fd; done) 2>&1
fi
[[ -f VERSION ]] && echo "bbport $(<VERSION)"
# GPU capability probe path (used by the launch planner and RADV detection below).
if [[ -n ${BB_PROBE:-} ]]; then caps=$(dirname -- "$BB_PROBE")/bb-gpu-capabilities
elif [[ -n ${BB_PREBUILT:-} ]]; then caps=bin/bb-gpu-capabilities
else caps=out/bb-gpu-capabilities; fi
# Native C++ Launch Planner (out/bb-launcher or bin/bb-launcher) runs with zero Python dependency;
# falls back to scripts/launch_plan.py only when bb-launcher is not built yet.
planner_bin=''
for candidate in out/bb-launcher bin/bb-launcher out/launcher/bb-launcher; do
    if [[ -x $candidate ]]; then planner_bin=$candidate; break; fi
done
plan_file=$out/launch.plan.$$
if [[ -n $planner_bin ]]; then
    "$planner_bin" --port-dir "$PWD" --data-dir "$data" --caps "$caps" --plan-file "$plan_file"
else
    if [[ -z ${PYTHON:-} ]]; then
        PYTHON=$(command -v python3 || true)
        if [[ -z $PYTHON ]]; then
            for candidate in /nix/store/*-python3-*/bin/python3; do
                if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
            done
        fi
    fi
    if [[ -z ${PYTHON:-} ]]; then echo 'Build out/bb-launcher (bash build.sh) or install Python 3.' >&2; exit 1; fi
    "$PYTHON" scripts/launch_plan.py --port-dir "$PWD" --data-dir "$data" --caps "$caps" --plan-file "$plan_file"
fi
mod_game='' bbsc_shadnet=''
probe_args=()
while IFS= read -r -d '' kind && IFS= read -r -d '' key && IFS= read -r -d '' val; do
    case $kind in
        SET) export "$key=$val" ;;
        UNSET) unset "$key" ;;
        META)
            case $key in
                mod_game) mod_game=$val ;;
                bbsc_shadnet) bbsc_shadnet=$val ;;
            esac
            ;;
        ARG) probe_args+=("$val") ;;
    esac
done < "$plan_file"
rm -f "$plan_file" 2>/dev/null || { [[ -n ${PYTHON:-} ]] && "$PYTHON" -c 'import os,sys; os.unlink(sys.argv[1])' "$plan_file" 2>/dev/null; } || true
if [[ -n $mod_game ]]; then
    trap 'rm -rf "$mod_game" 2>/dev/null || true' EXIT
fi
# GCN 3-5 under RADV (Polaris, Vega and their APUs): RADV_DEBUG=nohiz fixes depth compression artifacts.
gpu_vendor= gpu_name=
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} ]]; then
    IFS=$'\t' read -r gpu_vendor gpu_name < <("$caps" --device 2> /dev/null) || true
fi
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && $gpu_name == *"(RADV "* ]]; then
    for chip in TONGA ICELAND CARRIZO FIJI STONEY POLARIS VEGA RAVEN RENOIR; do
        if [[ $gpu_name == *"(RADV $chip"* ]]; then
            export RADV_DEBUG=nohiz
            echo "RADV: $gpu_name: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; BB_AUTO_NOHIZ=0: off)"
            break
        fi
    done
fi
# MangoHud deduplication (Steam Deck mangoapp or system implicit Vulkan layer vs bundled AppImage layer).
if [[ ${MANGOHUD:-0} == 1 ]]; then
    if pgrep -x mangoapp > /dev/null 2>&1; then
        echo "MangoHud: Steam's performance overlay is on; the in-game MangoHud stays off"
        unset MANGOHUD
        export DISABLE_MANGOHUD=1
    elif grep -qs '"VK_LAYER_MANGOHUD' /usr/share/vulkan/implicit_layer.d/*.json \
            /etc/vulkan/implicit_layer.d/*.json \
            "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json; then
        export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:+$VK_LOADER_LAYERS_DISABLE,}VK_LAYER_MANGOHUD_overlay_64_x86_64
    fi
fi
# FSR 4: faster post passes next to the downloaded ones (incremental; tools/fsr4_optimize.sh).
if [[ -z ${BB_PREBUILT:-} && -d fsr4_shaders ]] && command -v spirv-cross >/dev/null; then
    bash tools/fsr4_optimize.sh || echo 'FSR 4: optimized post passes not built' >&2
fi
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/bb-probe}
else
    bash build.sh
    probe=${BB_PROBE:-out/bb-probe}
fi
# Local BBSC co-op game server watcher when launch_plan.py selected 127.0.0.1 fallback.
if [[ -n $bbsc_shadnet && -f scripts/bbsc_gameserver/bbsc_gameserver.py ]]; then
    bbsc_dir=${BB_USER_DIR:-$data/user}/bbsc
    mkdir -p "$bbsc_dir/logs"
    game_pid=$$
    if ! (exec 3<>/dev/tcp/127.0.0.1/20443) 2>/dev/null; then
        ( "$PYTHON" scripts/bbsc_gameserver/bbsc_gameserver.py --shadnet "$bbsc_shadnet" \
              --host 127.0.0.1 --port 20443 --relay-port 0 --no-bbhost --no-playlog \
              --data "$bbsc_dir/online-data.json" --log-dir "$bbsc_dir/logs" \
              > "$bbsc_dir/logs/gameserver-console.log" 2>&1 &
          server_pid=$!
          while kill -0 "$game_pid" 2>/dev/null && kill -0 "$server_pid" 2>/dev/null; do sleep 2; done
          kill "$server_pid" 2>/dev/null ) &
        echo "Co-op: BBSC game server on 127.0.0.1:20443 (summons -> $bbsc_shadnet:31315)"
    else
        echo "Co-op: a game server answers on 127.0.0.1:20443 already: used"
    fi
fi
# Steam overlay (#77, #217): loaded for the game process alone.
if [[ -n ${BB_STEAM_OVERLAY:-} && $BB_STEAM_OVERLAY != 0 && -f $BB_STEAM_OVERLAY ]]; then
    export LD_PRELOAD=$BB_STEAM_OVERLAY ENABLE_VK_LAYER_VALVE_steam_overlay_1=1
    unset DISABLE_VK_LAYER_VALVE_steam_overlay_1
    echo "Steam overlay: $BB_STEAM_OVERLAY"
fi
probe_args+=("$@")
if [[ -n $mod_game ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    exit "$mod_status"
fi
exec "$probe" "${probe_args[@]}"
