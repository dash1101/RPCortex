#!/usr/bin/env bash
# Boot RPCortex under Renode, on an emulated RP2040.
#
# This exists because too many bugs have been found by the person holding the
# board rather than by the person writing the code — a stuck boot screen, a
# crash on `kill`, a panel that never lit. None of those needed hardware to
# reproduce. They needed somewhere to run the real firmware and watch it.
#
#     emu/run.sh              boot and print the console
#     emu/run.sh 40           boot, run for 40 seconds
#
# WHAT IT IS NOT. The model is an RP2040, so: no CYW43, therefore no WiFi and no
# Bluetooth; 264 KB of RAM rather than 520, so Nova D1's ~61 KB image is tight
# and may not load; and no USB, which is why the emulator build also puts the
# console on UART0. Everything else — the scheduler, both cores, littlefs on
# real emulated flash, the loader, the shell, packages — is the same code the
# board runs.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
V2="$(dirname "$HERE")"
SECS="${1:-25}"

PLAT="$HERE/Renode_RP2040"
if [ ! -d "$PLAT" ]; then
    echo "Fetching the RP2040 platform (matgla/Renode_RP2040)..."
    git clone --depth 1 https://github.com/matgla/Renode_RP2040.git "$PLAT" || exit 1
fi

# The platform's peripherals are C#, compiled once into a DLL that Renode loads.
DLL="$PLAT/emulation/bin/Release/netstandard2.1/Peripherals.dll"
if [ ! -f "$DLL" ]; then
    echo "Building the RP2040 peripherals (needs dotnet-sdk-8.0)..."
    dotnet build "$PLAT/emulation/Peripherals.csproj" -c Release || exit 1
fi

ELF="$V2/os/build_pico-emu/rpcortex_v2.elf"
if [ ! -f "$ELF" ]; then
    echo "Building the emulator image..."
    (cd "$V2" && ./build.sh pico --emu) || exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/boot.resc" <<RESC
\$machine_name?="rpcortex"
\$platform_file?=@$PLAT/boards/raspberry_pico.repl
include @$PLAT/boards/initialize_custom_board.resc
sysbus LoadELF @$ELF
sysbus.cpu0 VectorTableOffset 0x00000000
sysbus.cpu1 VectorTableOffset 0x00000000
sysbus.uart0 CreateFileBackend @$OUT/console.txt true
start
RESC

echo "Booting for ${SECS}s..."
timeout $((SECS + 30)) renode --disable-xwt --console --plain \
    -e "include @$OUT/boot.resc" -e "sleep $SECS" -e "quit" > "$OUT/renode.log" 2>&1

echo "--------------------------------------------------------------------"
# Strip the colour so the output is readable in a log and diffable between runs.
tr -d '\000' < "$OUT/console.txt" 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'
echo "--------------------------------------------------------------------"
grep -icE '\[error\]' "$OUT/renode.log" | xargs -I{} echo "renode errors: {}"
