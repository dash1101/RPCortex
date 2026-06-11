#!/usr/bin/env bash
# RPCortex Deploy Script
# Copies OS files to a connected MicroPython device using mpremote.
# Can deploy from the source tree (default) or a compiled dist/ image.
#
# Usage:
#   ./deploy.sh                        # deploy source from repo root
#   ./deploy.sh --compiled             # deploy compiled dist/ image
#   ./deploy.sh --compiled --out /path # deploy a custom dist directory
#   ./deploy.sh --port /dev/ttyUSB0   # specify serial port explicitly
#
# mpremote install:
#   pip install mpremote
#
# Notes:
#   Run compile.sh first if deploying --compiled.
#   /Nebula/ and /Users/ on the device are never touched — user data is safe.
#   If the device has no /Nebula/ yet (fresh flash), mpremote mkdir is silent.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${REPO_DIR}/dist"
PORT_ARG=""
COMPILED=0

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiled)  COMPILED=1;          shift ;;
        --out)       DIST_DIR="$2";       shift 2 ;;
        --port)      PORT_ARG="connect $2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0" | grep '^#' | sed 's/^# \?//'
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
if ! command -v mpremote &>/dev/null; then
    echo ""
    echo "  [!] mpremote not found in PATH."
    echo "      Install it with:  pip install mpremote"
    echo ""
    exit 1
fi

if (( COMPILED )); then
    SRC_DIR="${DIST_DIR}"
    if [[ ! -d "${SRC_DIR}" ]]; then
        echo "  [!] Compiled dist not found: ${SRC_DIR}"
        echo "      Run compile.sh first, or use --out to specify a path."
        exit 1
    fi
    echo ""
    echo "  Deploying COMPILED image from: ${SRC_DIR}"
else
    SRC_DIR="${REPO_DIR}"
    echo ""
    echo "  Deploying SOURCE from: ${SRC_DIR}"
fi

echo "  Port: ${PORT_ARG:-auto-detect}"
echo ""

# mpremote prefix (with optional explicit port)
MPR="mpremote ${PORT_ARG}"

# ---------------------------------------------------------------------------
# Copy the whole OS tree in ONE mpremote session.
#
# The old script ran a separate `mpremote cp` per file; each invocation pays
# the full connect + raw-REPL handshake (~1-2s), so 30+ files took a minute+.
# `cp -r <dir> :` recurses and creates the directory on the device; chaining
# with `+` keeps it all in a SINGLE raw-REPL session -- a huge speedup. It also
# copies ALL package dirs (PicoFetch, RPCMark, NTP, ...), which the per-file
# version missed.
#
# Only Core/, Packages/, and main.py are touched -- /Nebula/ and /Users/
# (user data) are never sent, so they're left intact.
# ---------------------------------------------------------------------------
echo "  -- Copying OS in one session: Core/, Packages/, main.py --"
echo "     (user data under /Nebula/ and /Users/ is left untouched)"
echo ""

if ${MPR} cp -r "${SRC_DIR}/Core" : + cp -r "${SRC_DIR}/Packages" : + cp "${SRC_DIR}/main.py" :; then
    echo ""
    echo "  ─────────────────────────────────────────────"
    echo "  Deploy complete. Reboot to apply:  ${MPR} reset"
    echo ""
    exit 0
else
    echo ""
    echo "  [!] Deploy failed."
    echo "      - Is the board plugged in? Try:  ./deploy.sh --port /dev/ttyACM0"
    echo "      - Close any other serial program (PuTTY/Thonny) using the port."
    exit 1
fi
