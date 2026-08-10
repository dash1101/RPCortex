#!/usr/bin/env python3
"""Copy a file to a running RPCortex v2 device over the serial console.

    tools/putfile.py build/apps/greet.app
    tools/putfile.py greet.app --name /pkg/greet.app
    tools/putfile.py firmware.bin --port /dev/ttyACM1

The device's `put <name> <len> [sha256]` reads raw bytes off the console. This
is the one sender: it does the three things an ad-hoc script gets wrong, each of
which has cost a corrupt transfer.

  DTR. pico_stdio_usb only counts the console as connected once the host asserts
  it. Without it output is dropped on the floor and the transfer corrupts with
  nothing to show for it.

  FLOW CONTROL. The device writes each chunk to flash before acknowledging it,
  and a flash write is slow and parks the other core. This waits for that
  acknowledgement — one 0x06 per chunk — so there is never more than one chunk
  in flight and nothing can arrive while the device cannot take it.

  THE DIGEST. The sha256 is computed here and passed to `put`, so the device
  hashes what actually landed and compares. A mismatch means the file is removed
  on the device and this exits non-zero. A transfer that is not verified is a
  transfer that is not finished.

Exit status is 0 only when the device reported a digest matching the file.
"""
import argparse
import hashlib
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is needed:  pip install pyserial")

# Must match PUT_CHUNK in os/shell/shell.cpp. The device acknowledges once per
# this many bytes written, so a sender using a different size waits for an
# acknowledgement that is not coming, or sends past one that has not arrived.
CHUNK = 256
ACK = 0x06

ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def read_until(port, needle, timeout, sink=None):
    """Read until `needle` appears, or the timeout runs out. Returns the text."""
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = port.read(4096)
        if chunk:
            buf.extend(chunk)
            if sink is not None:
                sink.extend(chunk)
            if needle in ANSI.sub(b"", bytes(buf)):
                break
    return ANSI.sub(b"", bytes(buf)).decode("utf-8", "replace")


def wait_ack(port, timeout):
    """One 0x06 from the device. False if it never came."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if b[0] == ACK:
            return True
        # Anything else is the device talking rather than acknowledging — an
        # error message, most likely. Keep looking until the timeout so the
        # caller can print what it said.
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="the local file to send")
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial port (default %(default)s)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--name", help="name on the device (default: the file's basename)")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="seconds to wait for one acknowledgement (default %(default)s)")
    ap.add_argument("--quiet", action="store_true", help="no progress line")
    args = ap.parse_args()

    try:
        data = open(args.file, "rb").read()
    except OSError as e:
        sys.exit("cannot read {}: {}".format(args.file, e))
    if not data:
        sys.exit("{} is empty; there is nothing to send".format(args.file))

    name = args.name or os.path.basename(args.file)
    digest = hashlib.sha256(data).hexdigest()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        sys.exit("cannot open {}: {}".format(args.port, e))

    # Before anything else. Without it the device does not consider the console
    # connected and its own output goes nowhere.
    port.setDTR(True)
    time.sleep(0.3)
    port.reset_input_buffer()

    print("{}  {} bytes  ->  {}:{}".format(args.file, len(data), args.port, name))

    port.write(b"\r")
    port.flush()
    time.sleep(0.4)
    port.reset_input_buffer()

    port.write("put {} {} {}\r".format(name, len(data), digest).encode())
    port.flush()

    # The device prints this once it is ready to read raw bytes. Waiting for it
    # rather than sleeping is what makes the first chunk safe: nothing is on the
    # wire while another task could still be reading the console.
    banner = read_until(port, b"raw bytes now", 10.0)
    if "raw bytes now" not in banner:
        print(banner.strip())
        sys.exit("the device never asked for the bytes — is it at a root shell?")

    sent = 0
    started = time.time()
    while sent < len(data):
        block = data[sent:sent + CHUNK]
        port.write(block)
        port.flush()
        sent += len(block)
        if not wait_ack(port, args.timeout):
            trailing = read_until(port, b"\x00", 1.0)
            print(trailing.strip())
            sys.exit("no acknowledgement after {} of {} bytes"
                     .format(sent, len(data)))
        if not args.quiet and (sent % (CHUNK * 200) == 0 or sent == len(data)):
            pct = sent * 100 // len(data)
            print("  {:3d}%  {} / {}".format(pct, sent, len(data)), flush=True)

    took = time.time() - started
    report = read_until(port, b"sha256", 30.0)
    print(report.strip())

    if "was not kept" in report or "sha256" not in report:
        sys.exit("the device refused the file")
    if digest not in report:
        sys.exit("the device reported a different digest — expected {}".format(digest))

    print("verified  {}  ({:.1f} s, {:.1f} KB/s)"
          .format(digest, took, len(data) / 1024.0 / max(took, 0.001)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
