#!/usr/bin/env bash
# Build the Nova D1 simulator: the package and its runner as a WebAssembly module.
#
#   ./build_sim.sh [out.wasm]            default: ./novad1-sim.wasm
#
# Needs a C++ compiler that targets wasm32-wasi. Zig bundles one and installs from
# pip, so nothing is needed system-wide:
#
#   python3 -m venv .zigenv && .zigenv/bin/pip install ziglang
#   ZIG=".zigenv/bin/python -m ziglang" ./build_sim.sh
#
# ZIG defaults to `zig` when that is on PATH. The output is one self-contained
# module with no host imports beyond WASI's handful (see NovaLabs js/sim.js for the
# shim), so it can sit beside a page and be fetched.
set -eu
cd "$(dirname "$0")"

OUT="${1:-novad1-sim.wasm}"
if [ -z "${ZIG:-}" ]; then
    if command -v zig >/dev/null 2>&1; then ZIG=zig
    else echo "build_sim.sh: set ZIG to a zig command (see the header)"; exit 1; fi
fi

# What the page says it is running: the package version, and the build the source
# was at. Taken from the tree, so it cannot be left stale by hand.
PKG_VER=$(sed -n 's/^#define NOVAD1_VER "\(.*\)"/\1/p' ../apps/novad1/novad1.cpp | head -1)
BUILD=$(git -C .. rev-list --count HEAD 2>/dev/null || echo 0)
COMMIT=$(git -C .. rev-parse --short HEAD 2>/dev/null || echo unknown)
DIRTY=$(git -C .. diff --quiet HEAD -- . 2>/dev/null && echo "" || echo "+")

# -Os: the module is downloaded by a visitor, and the panel is 128x64.
# No exceptions or RTTI, for the same reason the package builds without them.
$ZIG c++ -target wasm32-wasi -mexec-model=reactor \
    -std=c++17 -Os -fno-exceptions -fno-rtti -w \
    "-DSIM_STAMP=\"Nova D1 ${PKG_VER}, RPCortex v2 build ${BUILD} (${COMMIT}${DIRTY})\"" \
    -I.. -I../include \
    sim_wasm.cpp -o "$OUT" \
    -Wl,--strip-all -Wl,--no-entry

echo "built $OUT ($(wc -c < "$OUT") bytes)"
