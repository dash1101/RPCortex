#!/usr/bin/env bash
# Run the emulator test suite. See emu/tests/boot.robot.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
V2="$(dirname "$HERE")"

[ -d "$HERE/Renode_RP2040" ] || { echo "Run emu/run.sh once first."; exit 1; }
[ -f "$V2/os/build_pico-emu/rpcortex_v2.elf" ] || (cd "$V2" && ./build.sh pico --emu) || exit 1

# Renode's Robot runner needs its own Python dependencies, which are not the
# ones Renode itself installs. A venv beside the platform keeps them out of the
# system and out of the repo.
VENV="$HERE/.venv"
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV" || exit 1
    "$VENV/bin/pip" install -q -r /opt/renode/tests/requirements.txt || exit 1
fi

PATH="$VENV/bin:$PATH" renode-test "$HERE/tests/boot.robot" "$@"
