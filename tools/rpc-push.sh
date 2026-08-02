#!/usr/bin/env bash
# Copy a file to a running RPCortex v2 device over the serial console.
#
#   tools/rpc-push.sh build/apps/greet.app            # default port
#   tools/rpc-push.sh greet.app /dev/ttyACM0
#   tools/rpc-push.sh greet.app /dev/ttyACM0 /pkg/greet.app
#
# The device's `put <name> <len>` command reads exactly <len> raw bytes off the
# console. Typing that by hand means knowing the byte count, so this works it out
# and streams the file — which is the difference between a transfer being
# possible and it being the thing you actually do.
#
# Requires a logged-in shell on the device. Nothing here needs mpremote: the
# protocol is the shell's own command, not a REPL.

set -euo pipefail

FILE="${1:?usage: rpc-push.sh <file> [port] [remote-name]}"
PORT="${2:-/dev/ttyACM0}"
NAME="${3:-$(basename "$FILE")}"

[ -r "$FILE" ] || { echo "cannot read $FILE" >&2; exit 1; }
[ -w "$PORT" ] || { echo "cannot write $PORT (is the device plugged in, and are you in the dialout group?)" >&2; exit 1; }

LEN=$(stat -c%s "$FILE")
echo "==> $NAME  ($LEN bytes)  ->  $PORT"

# Raw mode: no echo, no CR/LF translation, no flow control. Without this the
# terminal driver mangles binary payloads — a 0x11/0x13 byte in the middle of an
# ELF becomes XON/XOFF and the transfer stalls with no error.
stty -F "$PORT" 115200 raw -echo -echoe -echok -crtscts -ixon -ixoff

# The device echoes as it goes; drain that in the background so the port does not
# fill and block the write.
cat "$PORT" > /dev/null &
DRAIN=$!
trap 'kill $DRAIN 2>/dev/null || true' EXIT

printf 'put %s %s\r' "$NAME" "$LEN" > "$PORT"
sleep 0.4                       # let the device print its prompt and start reading
cat "$FILE" > "$PORT"
sleep 0.3
printf '\r' > "$PORT"

echo "    sent. On the device:  run $NAME     (or: pkg install $NAME)"
