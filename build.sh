#!/usr/bin/env bash
# Build the RPCortex v2 firmware images.
#
#   ./build.sh              both boards
#   ./build.sh pico2_w      one board
#   ./build.sh --clean      wipe the build directories first
#
# Output lands in out/rpcortex-v2-<board>.uf2. Flashing is drag-and-drop: hold
# BOOTSEL, plug in, copy the .uf2 onto the RPI-RP2 drive. There is no rawrepl
# paste step any more — the whole OS is one image.

set -euo pipefail
cd "$(dirname "$0")"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$PWD/sdk}"
if [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    echo "pico-sdk not found at $PICO_SDK_PATH" >&2
    echo "Clone it there, or set PICO_SDK_PATH." >&2
    exit 1
fi

# Wireless needs these two SDK submodules; without them a W-board build fails
# deep inside lwIP headers rather than saying what is missing.
for sub in lib/cyw43-driver lib/lwip; do
    if [ ! -e "$PICO_SDK_PATH/$sub/CMakeLists.txt" ] && [ -z "$(ls -A "$PICO_SDK_PATH/$sub" 2>/dev/null)" ]; then
        echo "SDK submodule $sub is empty - fetching..."
        git -C "$PICO_SDK_PATH" submodule update --init "$sub"
    fi
done

CLEAN=0
BOARDS=()
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        -*)      echo "unknown option: $arg" >&2; exit 1 ;;
        *)       BOARDS+=("$arg") ;;
    esac
done
[ ${#BOARDS[@]} -eq 0 ] && BOARDS=(pico2_w pico_w)

mkdir -p out
for board in "${BOARDS[@]}"; do
    dir="os/build_$board"
    [ "$CLEAN" -eq 1 ] && rm -rf "$dir"
    echo "==> $board"
    cmake -S os -B "$dir" -DPICO_BOARD="$board" >/dev/null
    cmake --build "$dir" -j"$(nproc)" >/dev/null
    cp "$dir/rpcortex_v2.uf2" "out/rpcortex-v2-$board.uf2"
    size=$(stat -c%s "out/rpcortex-v2-$board.uf2")
    printf '    out/rpcortex-v2-%s.uf2  (%s KB)\n' "$board" "$((size / 1024))"
done

echo
echo "Host tests:"
os/host/run_all.sh
