#!/usr/bin/env bash
# Build the RPCortex v2 firmware images.
#
#   ./build.sh              both boards
#   ./build.sh pico2_w      one board
#   ./build.sh --clean      wipe the build directories first
#   ./build.sh --no-dev-packages   leave out bench, probe and stress
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
DEVPKGS=ON
BOARDS=()
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        # bench, probe and stress are baked into the image so a board can be
        # diagnosed before it has a network. They are of no use to somebody who
        # just wants the OS, so a shipping build leaves them out — and this is
        # the whole of doing that, rather than picking them out of the build.
        --no-dev-packages) DEVPKGS=OFF ;;
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
    # --clean wipes every os/build_* directory, which is where the only copy
    # used to live — so a clean build had nothing to find and fell back to
    # fetching from git, which fails outright without a network. The spike's
    # build directories are not cleaned by this script and hold the same
    # version, so they are searched too.
    PT=""
    for d in os/build_*/_deps/picotool loader-spike/build*/_deps/picotool; do
        [ -x "$d/picotool" ] && PT="$(cd "$d" && pwd)" && break
    done
    if [ -n "$PT" ]; then
        cmake -S os -B "$dir" -DPICO_BOARD="$board" -DRPC_DEV_PACKAGES="$DEVPKGS" \
              -Dpicotool_DIR="$PT" >/dev/null
    else
        cmake -S os -B "$dir" -DPICO_BOARD="$board" -DRPC_DEV_PACKAGES="$DEVPKGS" >/dev/null
    fi
    cmake --build "$dir" -j"$(nproc)" >/dev/null
    cp "$dir/rpcortex_v2.uf2" "out/rpcortex-v2-$board.uf2"
    size=$(stat -c%s "out/rpcortex-v2-$board.uf2")
    printf '    out/rpcortex-v2-%s.uf2  (%s KB)\n' "$board" "$((size / 1024))"

    # The firmware-writing routine must be in RAM, with every call it makes also
    # in RAM — it runs while the flash holding everything else is being erased.
    # Checked in the built image rather than trusted from the annotations,
    # because the two ways to get this wrong are both invisible in the source.
    python3 "$(dirname "$0")/tools/check-flashsafe.py" \
        "$dir/rpcortex_v2.elf" arm-none-eabi-nm arm-none-eabi-objdump || exit 1

    # And that every stack switch lets go of the stack limit before it moves SP.
    # On ARMv8-M, writing SP below MSPLIM is itself an overflow — it hard-faults
    # on the switch instruction, on the first switch into the shell, and the
    # device boot-loops with no shell left to report from. There is no MSPLIM on
    # a host, so no scheduler test can see it.
    python3 "$(dirname "$0")/tools/check-stackswitch.py" \
        "$dir/rpcortex_v2.elf" || exit 1

    # The raw image too, for OTA. A .uf2 wraps every 256 bytes in a 512-byte
    # block with a header, which is right for the boot ROM's drag-and-drop and
    # pure overhead for an update that writes flash directly.
    if [ -f "$dir/rpcortex_v2.bin" ]; then
        cp "$dir/rpcortex_v2.bin" "out/rpcortex-v2-$board.bin"
        bsize=$(stat -c%s "out/rpcortex-v2-$board.bin")
        printf '    out/rpcortex-v2-%s.bin  (%s KB)  sha256 %s\n' \
            "$board" "$((bsize / 1024))" \
            "$(sha256sum "out/rpcortex-v2-$board.bin" | cut -c1-16)..."

        # DOES IT STILL FIT?
        #
        # The firmware runs from the first half of RPC_FW_RESERVE and an update
        # is assembled in the second. Nothing checked that the image fits the
        # half it runs from, and the way that failure presents is in
        # storage.cpp's own words: "an image that outgrew its reserve mid-update
        # would be discovered by overwriting the start of the filesystem."
        # Finding out by losing the filesystem is not finding out.
        #
        # Reported every build, not only when it fails, because the number
        # matters most while it is still shrinking. It went from 71 KB of
        # headroom to 22 without anyone noticing.
        # RPC_FW_RESERVE is 2 MB and the slot is half of it. Kept in step with
        # loader-spike/firmware/storage.cpp by hand; there is one definition
        # there and this is the only other place that needs the number.
        slot=$((2048 * 1024 / 2))
        left=$((slot - bsize))
        if [ "$left" -lt 0 ]; then
            printf '    [!] %s KB OVER the %s KB slot — this image cannot be flashed by an update\n' \
                "$(( -left / 1024 ))" "$((slot / 1024))"
            exit 1
        elif [ "$left" -lt $((64 * 1024)) ]; then
            printf '    [?] %s KB left in the %s KB slot\n' \
                "$((left / 1024))" "$((slot / 1024))"
        else
            printf '    %s KB left in the %s KB slot\n' \
                "$((left / 1024))" "$((slot / 1024))"
        fi
    fi
done

echo
echo "Host tests:"
os/host/run_all.sh
