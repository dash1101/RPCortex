#!/usr/bin/env bash
# Build the RPCortex v2 firmware images.
#
#   ./build.sh              every board
#   ./build.sh pico2_w      one board
#   ./build.sh --clean      wipe the build directories first
#   ./build.sh --features   what an image can be built with or without, and the cost
#   ./build.sh pico2_w --without bluetooth      a custom image: rpcortex-v2-pico2_w-nobt
#   ./build.sh pico2_w --without sd,peripherals (several at once, comma separated)
#   ./build.sh pico2_w --with devpkgs           (or --dev-packages) bench, probe, stress, havoc
#   ./build.sh --release           the standard images a release publishes (the default)
#   ./build.sh --fast              -O3 on every board, for comparison
#   ./build.sh --small             -Os on every board, for comparison
#   ./build.sh pico --emu          the emulator image (console on UART0)
#
# tools/buildconf.py asks which board and which features and runs the command.
#
# Output lands in out/rpcortex-v2-<board>.uf2. Flashing is drag-and-drop: hold
# BOOTSEL, plug in, copy the .uf2 onto the RPI-RP2 drive. There is no rawrepl
# paste step any more — the whole OS is one image.
#
# A CUSTOM IMAGE (any feature set other than the board's standard one) is named
# for how it differs, rpcortex-v2-pico2_w-nobt.uf2, and built in its own
# directory, os/build_pico2_w-nobt, so it is never mistaken for the standard
# image and switching back and forth does not rebuild everything. `ver` on the
# device says which it is, and tools/release.py refuses to publish one.

set -euo pipefail
cd "$(dirname "$0")"

# Answered before anything else, because it needs no SDK.
for arg in "$@"; do
    if [ "$arg" = "--features" ]; then
        exec python3 tools/features.py list
    fi
done

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
# What to add to or leave out of each board's standard image. The table of what
# exists, which boards can have it and what is on by default is
# tools/features.py; this only collects the names.
#
# The development packages are one of those features, and OFF by default.
# bench, probe, stress and havoc are development tools: they exist to break the
# OS and to measure it, and they are of no use to somebody who wants an
# operating system. Baked in they cost 42 KB of an image that has 43 KB of
# headroom on a Pico W. They are published to the package repository instead,
# so a board that wants them installs them.
WITH=""
WITHOUT=""
RELEASE=0
BUILDTYPE=""              # empty: os/CMakeLists.txt picks per part
BOARDS=()
CONSOLE_UART=OFF
SUFFIX=""
want=""                   # the option whose value is the next argument
for arg in "$@"; do
    if [ -n "$want" ]; then
        case "$want" in
            with)    WITH="$WITH,$arg" ;;
            without) WITHOUT="$WITHOUT,$arg" ;;
        esac
        want=""
        continue
    fi
    case "$arg" in
        --clean) CLEAN=1 ;;
        --with)        want=with ;;
        --with=*)      WITH="$WITH,${arg#--with=}" ;;
        --without)     want=without ;;
        --without=*)   WITHOUT="$WITHOUT,${arg#--without=}" ;;
        # The old spellings for the development packages, kept so scripts and
        # habits do not break.
        --dev-packages)    WITH="$WITH,devpkgs" ;;
        --no-dev-packages) WITHOUT="$WITHOUT,devpkgs" ;;
        # What a published image is: the board's standard features
        # (tools/features.py) and no dev packages. Passed explicitly rather than left to the cache, because a
        # build directory configured once keeps whatever it was configured with
        # — which is how a -Os default went unnoticed for a whole rebuild.
        --release) RELEASE=1 ;;
        --fast) BUILDTYPE=Release ;;      # -O3 everywhere, for comparing
        # The emulator's RP2040 model has no USB, so an --emu image puts the
        # console on the UART it does have. Built into its own directory so it
        # never gets confused with the image that goes on a board.
        --emu) CONSOLE_UART=ON; SUFFIX="-emu" ;;
        --small) BUILDTYPE=MinSizeRel ;;  # -Os everywhere, for comparing
        -*)      echo "unknown option: $arg" >&2; exit 1 ;;
        *)       BOARDS+=("$arg") ;;
    esac
done
if [ -n "$want" ]; then
    echo "--$want needs a list of features, like --$want sd,peripherals (./build.sh --features)" >&2
    exit 1
fi
if [ "$RELEASE" -eq 1 ] && [ -n "${WITH//,/}${WITHOUT//,/}" ]; then
    echo "--release builds the standard images; it cannot be combined with --with or --without" >&2
    exit 1
fi
# Every RP2 board this runs on. The wireless ones and the plain ones: a plain
# board is one on which Wi-Fi and Bluetooth cannot be on, and net_off.cpp and
# bt_off.cpp stand in for them, so the non-W images are a real build rather
# than a broken one.
#
# ESP32-S3 is NOT here. It is a different architecture with a different SDK —
# the pure core/ layer moves over unchanged, but the context switch, storage and
# the network layer all need Xtensa equivalents. Listing it as a target before
# any of that exists would be a lie in a build script.
[ ${#BOARDS[@]} -eq 0 ] && BOARDS=(pico2_w pico_w pico2 pico)

mkdir -p out
for board in "${BOARDS[@]}"; do
    # Which features, as CMake options, and the name a custom image carries.
    # Refused here, before any building, when the board cannot have something.
    feat=$(python3 tools/features.py resolve "$board" --with "$WITH" --without "$WITHOUT") || exit 1
    eval "$feat"
    VARIANT="${FEAT_TAG:+-$FEAT_TAG}"
    name="rpcortex-v2-${board}${VARIANT}${SUFFIX}"
    dir="os/build_${board}${VARIANT}${SUFFIX}"
    # What the image must say it is: the marker check below compares.
    expect="${FEAT_TAG}"
    [ -n "$SUFFIX" ] && expect="${expect:+$expect-}emu"
    [ -n "$expect" ] || expect="standard"
    case "$board" in
        pico|pico_w) board_flash_kb=2048 ;;
        *)           board_flash_kb=4096 ;;
    esac
    [ "$CLEAN" -eq 1 ] && rm -rf "$dir"
    echo "==> $board${VARIANT:+  (custom image: $FEAT_TAG)}"
    if [ -n "$FEAT_NOTES" ]; then
        printf '%s\n' "$FEAT_NOTES" | sed 's/^/    note: /'
    fi
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
    # Every feature option is passed, on or off, every time: a build directory
    # remembers what it was configured with, and one left to remember would
    # build the last feature set rather than this one.
    # shellcheck disable=SC2086
    if [ -n "$PT" ]; then
        cmake -S os -B "$dir" -DPICO_BOARD="$board" $FEAT_CMAKE \
            -DRPC_CONSOLE_UART="$CONSOLE_UART" \
              ${BUILDTYPE:+-DCMAKE_BUILD_TYPE="$BUILDTYPE"} -Dpicotool_DIR="$PT" >/dev/null
    else
        cmake -S os -B "$dir" -DPICO_BOARD="$board" $FEAT_CMAKE \
            -DRPC_CONSOLE_UART="$CONSOLE_UART" \
              ${BUILDTYPE:+-DCMAKE_BUILD_TYPE="$BUILDTYPE"} >/dev/null
    fi
    cmake --build "$dir" -j"$(nproc)" >/dev/null
    cp "$dir/rpcortex_v2.uf2" "out/$name.uf2"
    size=$(stat -c%s "out/$name.uf2")
    printf '    out/%s.uf2  (%s KB)\n' "$name" "$((size / 1024))"
    printf '    features: %s\n' "${FEAT_LIST:-none}"

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
        cp "$dir/rpcortex_v2.bin" "out/$name.bin"
        bsize=$(stat -c%s "out/$name.bin")
        printf '    out/%s.bin  (%s KB)  sha256 %s\n' \
            "$name" "$((bsize / 1024))" \
            "$(sha256sum "out/$name.bin" | cut -c1-16)..."

        # THE IMAGE SAYS WHAT IT IS. imagefeatures.cpp bakes "rpcimage:<variant>" into
        # every build, and tools/release.py publishes only "standard". Checked
        # here, on the image just built, so that what the file is called, what the
        # build was asked for and what the device will say cannot drift apart.
        got=$(python3 tools/features.py image-variant "out/$name.bin") || exit 1
        if [ "$got" != "$expect" ]; then
            printf '    [!] the image says it is "%s"; this build asked for "%s"\n' "$got" "$expect"
            exit 1
        fi

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
        # The slot is half of RPC_FW_RESERVE, which is per part — 2 MB on
        # RP2350, 1664 KB on RP2040, which holds 2 MB of flash in total and
        # needs room left over for a filesystem. Kept in step with
        # loader-spike/firmware/storage.cpp by hand; there is one definition
        # there and this is the only other place that needs the number.
        # READ FROM THE BUILD, not from a table here. os/CMakeLists.txt owns the
        # number and writes it out; a second copy in this script could disagree
        # with the firmware and nothing would notice — which is the exact
        # failure this check exists to catch.
        reserve=$(cat "$dir/fw_reserve.txt")
        slot=$((reserve / 2))
        # On RP2350 the image goes into an A/B partition, 4 KB smaller than half
        # the reserve (os/CMakeLists.txt writes it out).
        [ -f "$dir/fw_slot.txt" ] && slot=$(cat "$dir/fw_slot.txt")
        # The region packages RUN from sits between the reserve and the
        # filesystem, so it comes out of the filesystem and the filesystem figure
        # has to know. Read from the build for the same reason the reserve is: a
        # second copy of the number here could disagree with the firmware and
        # nothing would notice.
        pkgregion=0
        [ -f "$dir/pkg_region.txt" ] && pkgregion=$(cat "$dir/pkg_region.txt")
        fs=$(( (board_flash_kb * 1024) - reserve - pkgregion ))
        left=$((slot - bsize))
        if [ "$left" -lt 0 ]; then
            printf '    [!] %s KB OVER the %s KB slot — this image cannot be flashed by an update\n' \
                "$(( -left / 1024 ))" "$((slot / 1024))"
            exit 1
        elif [ "$left" -lt $((64 * 1024)) ]; then
            printf '    [?] %s KB left in the %s KB slot, %s KB filesystem\n' \
                "$((left / 1024))" "$((slot / 1024))" "$((fs / 1024))"
        else
            printf '    %s KB left in the %s KB slot, %s KB filesystem\n' \
                "$((left / 1024))" "$((slot / 1024))" "$((fs / 1024))"
        fi
        # Named separately rather than folded into the filesystem line, because a
        # quarter of a megabyte that used to be storage should be visible on
        # every build and not only to whoever reads the table in CMakeLists.
        if [ "$pkgregion" -gt 0 ]; then
            printf '    %s KB of package slots (out of the filesystem)\n' \
                "$((pkgregion / 1024))"
        fi
        if [ "$fs" -le 0 ]; then
            printf '    [!] no filesystem: the reserve and the slots are the whole of this flash\n'
            exit 1
        fi
        # WHERE those bytes went, by component. The slot line above says how much
        # is left; this says what is spending it, so a pass that grew the image
        # can be pointed at what grew rather than bisected for. A readout, never a
        # gate — `|| true` keeps a reporting hiccup from failing a good build.
        python3 "$(dirname "$0")/tools/size-report.py" "$dir/rpcortex_v2.elf.map" --groups 2>/dev/null || true
        # And whether it moved a lot since the size somebody last accepted. Only
        # for the standard image: a custom one is meant to differ, and recording
        # it as the baseline would hide the next real growth.
        if [ "$expect" = "standard" ]; then
            python3 "$(dirname "$0")/tools/size-guard.py" check "$board" "out/$name.bin" || true
        fi
    fi
done

echo
echo "Host tests:"
os/host/run_all.sh
