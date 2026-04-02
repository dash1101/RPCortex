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
# Helper: upload a single file
# ---------------------------------------------------------------------------
uploaded=0
errors=0

upload() {
    local local_path="$1"
    local remote_path="$2"
    local rel="${local_path#${SRC_DIR}/}"

    if ${MPR} cp "${local_path}" ":${remote_path}" 2>/dev/null; then
        printf "  \033[32m[+]\033[0m %s\n" "$rel"
        uploaded=$(( uploaded + 1 ))
    else
        printf "  \033[31m[!]\033[0m FAILED: %s\n" "$rel"
        errors=$(( errors + 1 ))
    fi
}

# Ensure remote directories exist (silent if already present)
ensure_dir() {
    ${MPR} mkdir ":$1" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Create remote directory tree
# ---------------------------------------------------------------------------
echo "  -- Creating remote directories --"
ensure_dir "/Core"
ensure_dir "/Core/Launchpad"
ensure_dir "/Packages"
ensure_dir "/Packages/Launchpad"
ensure_dir "/Packages/Editor"
echo ""

# ---------------------------------------------------------------------------
# main.py
# ---------------------------------------------------------------------------
echo "  -- Entry point --"
upload "${SRC_DIR}/main.py" "/main.py"
echo ""

# ---------------------------------------------------------------------------
# Core files
# ---------------------------------------------------------------------------
echo "  -- Core modules --"
# Source deploy: .py files; compiled deploy: .py (stubs) + .mpy files
for f in "${SRC_DIR}"/Core/*.py "${SRC_DIR}"/Core/*.mpy; do
    [[ -f "$f" ]] || continue
    fname="${f##*/}"
    upload "$f" "/Core/${fname}"
done
echo ""

# ---------------------------------------------------------------------------
# Launchpad command files
# ---------------------------------------------------------------------------
echo "  -- Launchpad --"
for f in "${SRC_DIR}"/Core/Launchpad/*; do
    [[ -f "$f" ]] || continue
    fname="${f##*/}"
    upload "$f" "/Core/Launchpad/${fname}"
done
echo ""

# ---------------------------------------------------------------------------
# Package stubs
# ---------------------------------------------------------------------------
echo "  -- Package stubs --"
for pkg_dir in Launchpad Editor; do
    for f in "${SRC_DIR}/Packages/${pkg_dir}"/*; do
        [[ -f "$f" ]] || continue
        fname="${f##*/}"
        upload "$f" "/Packages/${pkg_dir}/${fname}"
    done
done
echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "  ─────────────────────────────────────────────"
if (( errors > 0 )); then
    echo "  [!] ${errors} file(s) failed to upload — check output above."
fi
echo "  Uploaded  : ${uploaded} files"
echo ""
echo "  Deploy complete. Reboot the device to apply."
echo ""

(( errors > 0 )) && exit 1
exit 0
