#!/usr/bin/env bash
# Copy a file to a running RPCortex v2 device over the serial console.
#
#   tools/rpc-push.sh build/apps/greet.app            # default port
#   tools/rpc-push.sh greet.app /dev/ttyACM0
#   tools/rpc-push.sh greet.app /dev/ttyACM0 /pkg/greet.app
#
# A shim now. This used to blast the file at the port and hope, which is exactly
# how two 300 KB uploads arrived corrupt: it did not wait for each chunk to
# reach flash, and it had no way to tell whether what landed was what was sent.
# tools/putfile.py does both. Having two senders was the problem, so there is
# one, and this keeps the name working.

set -euo pipefail
cd "$(dirname "$0")/.."

FILE="${1:?usage: rpc-push.sh <file> [port] [remote-name]}"
PORT="${2:-/dev/ttyACM0}"
NAME="${3:-}"

ARGS=("$FILE" --port "$PORT")
[ -n "$NAME" ] && ARGS+=(--name "$NAME")

exec python3 tools/putfile.py "${ARGS[@]}"
