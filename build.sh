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
# Every RP2 board this runs on. The wireless ones and the plain ones: net.cpp
# compiles to "no radio on this board" stubs without the CYW43 part, so the
# non-W images are a real build rather than a broken one.
#
# ESP32-S3 is NOT here. It is a different architecture with a different SDK —
# the pure core/ layer moves over unchanged, but the context switch, storage and
# the network layer all need Xtensa equivalents. Listing it as a target before
# any of that exists would be a lie in a build script.
[ ${#BOARDS[@]} -eq 0 ] && BOARDS=(pico2_w pico_w pico2 pico)

mkdir -p out
for board in "${BOARDS[@]}"; do
    dir="os/build_$board"
    [ "$CLEAN" -eq 1 ] && rm -rf "$dir"
    echo "==> $board"
    # picotool: the system copy is 2.1.1 and the SDK requires 2.3.0, so a fresh
    # board directory refuses to configure while an existing one is fine. Point
    # every board at the copy the first build fetched instead of downloading it
    # again per board.
    PT=""
    for d in os/build_*/_deps/picotool; do
        [ -x "$d/picotool" ] && PT="$(cd "$d" && pwd)" && break
    done
    if [ -n "$PT" ]; then
        cmake -S os -B "$dir" -DPICO_BOARD="$board" -Dpicotool_DIR="$PT" >/dev/null
    else
        cmake -S os -B "$dir" -DPICO_BOARD="$board" >/dev/null
    fi
    cmake --build "$dir" -j"$(nproc)" >/dev/null
    cp "$dir/rpcortex_v2.uf2" "out/rpcortex-v2-$board.uf2"
    size=$(stat -c%s "out/rpcortex-v2-$board.uf2")
    printf '    out/rpcortex-v2-%s.uf2  (%s KB)\n' "$board" "$((size / 1024))"

    # The raw image too, for OTA. A .uf2 wraps every 256 bytes in a 512-byte
    # block with a header, which is right for the boot ROM's drag-and-drop and
    # pure overhead for an update that writes flash directly.
    if [ -f "$dir/rpcortex_v2.bin" ]; then
        cp "$dir/rpcortex_v2.bin" "out/rpcortex-v2-$board.bin"
        bsize=$(stat -c%s "out/rpcortex-v2-$board.bin")
        printf '    out/rpcortex-v2-%s.bin  (%s KB)  sha256 %s\n' \
            "$board" "$((bsize / 1024))" \
            "$(sha256sum "out/rpcortex-v2-$board.bin" | cut -c1-16)..."
    fi
done

echo
echo "Host tests:"
os/host/run_all.sh
