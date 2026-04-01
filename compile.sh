#!/usr/bin/env bash
# RPCortex OS Compiler
# Compiles Core/**/*.py -> .mpy using mpy-cross and writes a deploy-ready
# image to dist/ (or a custom directory).
#
# Usage:
#   ./compile.sh                      # defaults: armv6m arch, ./dist output
#   ./compile.sh --arch armv7m        # RP2350
#   ./compile.sh --arch xtensawin     # ESP32
#   ./compile.sh --out /tmp/rpc_built
#
# Arch reference:
#   armv6m    — RP2040  (Pico, Pico W)
#   armv7m    — RP2350  (Pico 2, Pico 2 W)
#   xtensawin — ESP32 / ESP32-S2 / ESP32-S3
#
# mpy-cross install:
#   pip install mpy-cross
#
# Notes:
#   main.py is ALWAYS copied as-is — MicroPython boots main.py, not main.mpy.
#   Core/rpc_stub.py is ALWAYS copied as-is — it is written as main.py at
#   reinstall time and must remain human-readable source.
#   All .lp command registry files are copied as-is (not Python).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${REPO_DIR}/dist"
ARCH="armv6m"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)  ARCH="$2";     shift 2 ;;
        --out)   DIST_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1  (run with --help for usage)"
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Prereq check
# ---------------------------------------------------------------------------
if ! command -v mpy-cross &>/dev/null; then
    echo ""
    echo "  [!] mpy-cross not found in PATH."
    echo "      Install it with:  pip install mpy-cross"
    echo "      Or download from: https://pypi.org/project/mpy-cross/"
    echo ""
    exit 1
fi

MPY_VER=$(mpy-cross --version 2>&1 | head -1)

echo ""
echo "  ╔═══════════════════════════════════════╗"
echo "  ║    RPCortex OS Compiler               ║"
echo "  ╚═══════════════════════════════════════╝"
echo ""
echo "  mpy-cross : ${MPY_VER}"
echo "  Arch      : ${ARCH}"
echo "  Output    : ${DIST_DIR}"
echo ""

# ---------------------------------------------------------------------------
# Clean and create output tree
# ---------------------------------------------------------------------------
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/Core/Launchpad"
mkdir -p "${DIST_DIR}/Packages/Launchpad"
mkdir -p "${DIST_DIR}/Packages/Editor"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
compiled=0
copied=0
errors=0
bytes_src=0
bytes_out=0

# Files that must NOT be compiled (stay as .py source)
SKIP_COMPILE=("main.py" "Core/rpc_stub.py")

is_skip() {
    local rel="${1#${REPO_DIR}/}"
    for s in "${SKIP_COMPILE[@]}"; do
        [[ "$rel" == "$s" ]] && return 0
    done
    return 1
}

compile_file() {
    local src="$1"
    local rel="${src#${REPO_DIR}/}"
    local dst_dir="${DIST_DIR}/$(dirname "$rel")"
    local base="${src##*/}"
    local stem="${base%.py}"
    local dst_mpy="${dst_dir}/${stem}.mpy"

    mkdir -p "$dst_dir"

    local sz_src=0
    sz_src=$(wc -c < "$src" 2>/dev/null || echo 0)

    local err_out
    if err_out=$(mpy-cross -march="${ARCH}" -o "${dst_mpy}" "${src}" 2>&1); then
        local sz_mpy=0
        sz_mpy=$(wc -c < "$dst_mpy" 2>/dev/null || echo 0)
        bytes_src=$(( bytes_src + sz_src ))
        bytes_out=$(( bytes_out + sz_mpy ))
        compiled=$(( compiled + 1 ))
        printf "  \033[32m[+]\033[0m %-52s %5d B -> %4d B\n" "$rel" "$sz_src" "$sz_mpy"
    else
        printf "  \033[31m[!]\033[0m FAILED: %s\n" "$rel"
        printf "      %s\n" "$err_out"
        errors=$(( errors + 1 ))
    fi
}

copy_file() {
    local src="$1"
    local rel="${src#${REPO_DIR}/}"
    local dst="${DIST_DIR}/${rel}"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
    copied=$(( copied + 1 ))
    printf "  \033[90m[~]\033[0m %-52s (source)\n" "$rel"
}

# ---------------------------------------------------------------------------
# main.py — always source
# ---------------------------------------------------------------------------
echo "  -- Entry point --"
copy_file "${REPO_DIR}/main.py"
echo ""

# ---------------------------------------------------------------------------
# Core/*.py
# ---------------------------------------------------------------------------
echo "  -- Core modules --"
while IFS= read -r -d '' f; do
    if is_skip "$f"; then
        copy_file "$f"
    else
        compile_file "$f"
    fi
done < <(find "${REPO_DIR}/Core" -maxdepth 1 -name "*.py" -print0 | sort -z)
echo ""

# ---------------------------------------------------------------------------
# Core/Launchpad/*.py
# ---------------------------------------------------------------------------
echo "  -- Launchpad command files --"
while IFS= read -r -d '' f; do
    compile_file "$f"
done < <(find "${REPO_DIR}/Core/Launchpad" -maxdepth 1 -name "*.py" -print0 | sort -z)
echo ""

# ---------------------------------------------------------------------------
# Non-Python assets (copied as-is)
# ---------------------------------------------------------------------------
echo "  -- Command registries & package configs --"
while IFS= read -r -d '' f; do
    copy_file "$f"
done < <(find "${REPO_DIR}/Core/Launchpad" -maxdepth 1 -name "*.lp" -print0 | sort -z)

# Package stub configs
for cfg_dir in "${REPO_DIR}/Packages/Launchpad" "${REPO_DIR}/Packages/Editor"; do
    if [[ -d "$cfg_dir" ]]; then
        while IFS= read -r -d '' f; do
            copy_file "$f"
        done < <(find "$cfg_dir" -maxdepth 1 -print0 | grep -z '\.' 2>/dev/null || true)
    fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "  ─────────────────────────────────────────────"

savings=0
if (( bytes_src > 0 )); then
    savings=$(( (bytes_src - bytes_out) * 100 / bytes_src ))
fi

if (( errors > 0 )); then
    echo "  \033[31m[!]\033[0m ${errors} file(s) failed to compile — check output above."
fi

echo "  Compiled  : ${compiled} files"
echo "  Copied    : ${copied}   files (source — not compiled)"
echo ""
echo "  Source    : ${bytes_src} bytes  (Python source)"
echo "  Output    : ${bytes_out} bytes  (~${savings}% smaller)"
echo ""
echo "  Image ready: ${DIST_DIR}"
echo ""

(( errors > 0 )) && exit 1
exit 0
