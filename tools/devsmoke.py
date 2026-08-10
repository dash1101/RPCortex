#!/usr/bin/env python3
"""One command that asks a real board whether the OS still works.

    tools/devsmoke.py                    # the board on /dev/ttyACM0
    tools/devsmoke.py --port /dev/ttyACM1
    tools/devsmoke.py --no-reboot        # start from the shell that is there

It reboots the board, checks the version, installs and runs a package,
brings the Nova D1 screen up, takes a picture of it, and reads the memory
and memory-protection reports back. Anything that does not hold prints a
line beginning FAIL and the exit status is 1.

WHY IT EXISTS
-------------
The host suites in os/host prove the arithmetic of the OS: that relocations
resolve to the right numbers, that a protection region can be encoded, that
a screen's layout is what the code says it is. None of them execute a
single instruction of package code on an ARM core, and none of them has an
MPU. Bug #103 was a package that loaded perfectly, resolved every
relocation correctly, and could not execute its first instruction, because
the region holding it was silently never programmed. Sixty-one host suites
were green. Only a board could have said otherwise.

This is that board, asked in one command. It is deliberately shallow: it
runs the things that break loudly, not the things that break subtly.

WHAT IT DOES NOT COVER
----------------------
Worth reading before trusting a pass.

  * Anything needing hardware attached. No display, no radio module, no
    card, no sensor. `novad1 shot` renders the framebuffer the driver would
    have pushed; it does not prove a panel lit up.
  * The network. The radio is left exactly as it was found, so packages are
    installed from files already on the device rather than from the
    repository. HTTPS, the package index and OTA are untested here.
  * Installing Nova D1. It is checked as already installed, loaded,
    running and rendering, but never installed here, because on a Pico 2 W
    running v2.0.0 it cannot be: `pkg install /novad1.app` needs 121728
    bytes for the read-only half in ONE piece, and the largest free block
    is 84 KB on a booted board and 108 KB after `novad1 service stop` and
    `unload novad1`. That is correct behaviour rather than a bug —
    relocation cannot work on a scattered image — but it does mean the
    install path is proved here by greet, a 1.4 KB package, and not by a
    260 KB one.
  * Timing. Nothing here measures jitter, flash-write duration or watchdog
    margin. `probe` is the tool for that and it wants reading, not
    asserting.
  * Anything cumulative. This is one pass of a few seconds. Leaks, heap
    fragmentation over days and wear are invisible to it.

REQUIREMENTS
------------
pyserial. The board must boot to a shell without a login prompt, or the
first command will be answered by the password prompt and every check will
fail at once.
"""

import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("devsmoke needs pyserial: pip install pyserial")


# The shell's prompt, e.g. "root@vela:/>". The line editor redraws the whole
# prompt on every keystroke, so this appears many times in the echo of one
# command as well as once at the end of its output.
PROMPT = re.compile(rb"[\w.-]+@[\w.-]+:[^\r\n]*>\s*$")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


class Board:
    """A serial console, with the shell's own echo taken back off."""

    def __init__(self, port, baud=115200, quiet=False):
        self.port = port
        self.baud = baud
        self.quiet = quiet
        self.ser = None

    def open(self, tries=40):
        # After a reboot the USB CDC device disappears and comes back, so
        # opening is a retry rather than a single attempt.
        last = None
        for _ in range(tries):
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.3)
                time.sleep(0.3)
                return
            except (OSError, serial.SerialException) as exc:
                last = exc
                time.sleep(0.5)
        raise SystemExit("cannot open {}: {}".format(self.port, last))

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def send(self, cmd, wait=20.0, settle=0.8):
        """Run one command and return only what it printed."""
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r").encode())
        self.ser.flush()
        buf = bytearray()
        start = last = time.time()
        while time.time() - start < wait:
            chunk = self.ser.read(4096)
            if chunk:
                buf.extend(chunk)
                last = time.time()
                continue
            clean = ANSI.sub(b"", bytes(buf))
            tail = clean.split(b"\n")[-1]
            if PROMPT.search(tail) and time.time() - last >= settle:
                break
        text = ANSI.sub(b"", bytes(buf)).decode("utf-8", "replace")
        # Everything the command PRINTED comes after the echo of the command
        # being submitted — which is the FIRST place the command text is
        # followed by a newline. The line editor redraws the prompt on every
        # keystroke, so the command appears many times before that, and
        # searching backwards instead finds the copy inside the output:
        # `greet` prints "hello from the greet package", and cutting at the
        # last "greet" left "package (argc=1)".
        m = re.search(re.escape(cmd) + r"\r?\n", text)
        if m:
            text = text[m.end():]
        # Then drop the prompt the shell wrote when it finished.
        lines = [ln.rstrip("\r") for ln in text.split("\n")]
        while lines and (not lines[-1].strip() or PROMPT.search(lines[-1].encode())):
            lines.pop()
        return "\n".join(ln for ln in lines if ln.strip()).strip()

    def wake(self):
        """Get to a clean prompt, whatever state the line editor is in."""
        self.ser.write(b"\r")
        self.ser.flush()
        time.sleep(0.6)
        self.ser.reset_input_buffer()

    def reboot(self, wait=60.0):
        """Restart the board and come back when the shell answers again.

        `reboot` is the safe recovery for this OS and the one thing that is
        always worth trying first. The USB device goes away with it, so the
        port is closed and reopened rather than read through the gap.
        """
        try:
            self.ser.write(b"reboot\r")
            self.ser.flush()
        except (OSError, serial.SerialException):
            pass
        self.close()
        time.sleep(3.0)
        deadline = time.time() + wait
        # The device node reappearing is not the same as the console being
        # ready: udev creates it as soon as the CDC interface enumerates, and
        # a write in the gap fails with EIO. So every step here is allowed to
        # throw, and throwing means close it and try again.
        while time.time() < deadline:
            if not os.path.exists(self.port):
                time.sleep(0.5)
                continue
            try:
                if self.ser is None:
                    self.ser = serial.Serial(self.port, self.baud, timeout=0.3)
                    time.sleep(0.5)
                self.wake()
                if "uptime" in self.send("help system", wait=8.0).lower():
                    return True
            except (OSError, serial.SerialException):
                self.close()
            time.sleep(1.0)
        return False


class Report:
    """Checks, in the order they were asked, and whether each held."""

    def __init__(self):
        self.failed = 0
        self.passed = 0

    def ok(self, what, detail=""):
        self.passed += 1
        print("  ok   {:<34} {}".format(what, detail))

    def fail(self, what, detail=""):
        self.failed += 1
        print("  FAIL {:<34} {}".format(what, detail))

    def note(self, text):
        print("       {}".format(text))

    def check(self, cond, what, detail="", fail_detail=None):
        if cond:
            self.ok(what, detail)
        else:
            self.fail(what, detail if fail_detail is None else fail_detail)
        return bool(cond)


# --- the checks ---------------------------------------------------------


def check_version(b, r):
    out = b.send("ver")
    m = re.search(r"RPCortex v(\d+\.\d+\.\d+)", out)
    build = re.search(r"Build:\s*(\d+)", out)
    plat = re.search(r"Platform:\s*(\S+)", out)
    if not m:
        r.fail("ver reports a version", out.replace("\n", " | ")[:90])
        return None
    r.ok("ver reports a version",
         "v{}  build {}  {}".format(m.group(1),
                                    build.group(1) if build else "?",
                                    plat.group(1) if plat else "?"))
    return m.group(1)


def check_install_and_run(b, r, path, name, expect):
    """A real install round trip, then run what it installed.

    This is the whole loader on real silicon: read the ELF off littlefs,
    place two halves on the heap, resolve every relocation, write the
    veneers, program five protection regions, drop privilege and call the
    package's registered command. A host test can check every number in
    that sentence and not one of the verbs.
    """
    out = b.send("pkg install {}".format(path), wait=90.0)
    if "Installed" not in out and "loaded" not in out:
        r.fail("pkg install {}".format(name), out.replace("\n", " | ")[:110])
        return False
    r.ok("pkg install {}".format(name),
         "from {}".format(path))
    out = b.send(name, wait=30.0)
    return r.check(expect in out, "{} runs and prints".format(name),
                   out.replace("\n", " | ")[:90])


def check_novad1_service(b, r):
    listed = b.send("pkg list", wait=30.0)
    if "novad1" not in listed:
        r.fail("novad1 is installed",
               "not in 'pkg list' — install it before running this")
        return False
    r.ok("novad1 is installed", "")

    loaded = b.send("apps", wait=30.0)
    m = re.search(r"novad1\s+(\d+)\s*B", loaded)
    r.check(m is not None, "novad1 is loaded",
            "{} B resident".format(m.group(1)) if m else loaded[:80])

    b.send("novad1 service start", wait=45.0)
    time.sleep(1.5)
    status = b.send("novad1 service status", wait=30.0)
    return r.check("running" in status, "novad1 service is running",
                   status.split("\n")[0][:80] if status else "(no answer)")


def check_shot(b, r):
    """Take a picture of the screen and insist there is something on it.

    A screen that draws nothing is, on the device, indistinguishable from a
    hang: the runner keeps its frame loop, the shell keeps answering, and
    the panel is simply blank. So the frame is measured rather than
    glanced at — the right shape, and enough lit pixels to be a picture
    rather than an artefact.
    """
    out = b.send("novad1 shot", wait=45.0)
    head = re.search(r"(\d+)x(\d+)\s+depth\s+(\d+)", out)
    if not head:
        r.fail("novad1 shot renders a frame", out.replace("\n", " | ")[:90])
        return False
    w, h = int(head.group(1)), int(head.group(2))
    rows = [ln for ln in out.split("\n") if len(ln) == w and set(ln) <= set(".#")]
    lit = sum(ln.count("#") for ln in rows)
    if len(rows) < h * 9 // 10:
        r.fail("novad1 shot renders a frame",
               "{}x{} claimed, {} full rows came back".format(w, h, len(rows)))
        return False
    # One per cent of the panel. A home screen is nearer ten; the floor is
    # set low so that a redesign does not fail this, and a blank or
    # nearly-blank frame still does.
    floor = (w * h) // 100
    return r.check(lit >= floor, "novad1 shot renders a frame",
                   "{}x{}, {} rows, {} pixels lit".format(w, h, len(rows), lit))


def check_meminfo(b, r):
    out = b.send("meminfo", wait=30.0)
    def kb(label):
        m = re.search(label + r"\s*:?\s*(\d+)\s*KB", out)
        return int(m.group(1)) if m else None
    total, free, largest = kb("Total"), kb("Free"), kb("Largest block")
    if None in (total, free, largest):
        r.fail("meminfo reads back", out.replace("\n", " | ")[:90])
        return False
    ok = 0 < free <= total and 0 < largest <= free
    r.check(ok, "meminfo is self-consistent",
            "total {} KB, free {} KB, largest block {} KB".format(total, free, largest))
    # The number that decides whether anything else can be installed. A
    # device does not fail on the total; it fails when one request is
    # bigger than the largest free block.
    return r.check(largest >= 16, "the heap is not shredded",
                   "largest block {} KB".format(largest))


def check_mpu(b, r):
    """What the protection hardware is actually holding, and whether
    anything faulted while this ran.

    The fault handler runs on a stack of its own. If its high-water mark is
    still zero at the end of a pass that loaded a package, dropped
    privilege and ran its code, then nothing faulted — which is the one
    thing no host test can say.
    """
    out = b.send("mpu", wait=30.0)
    regions = re.search(r"Regions available\s*:\s*(\d+)", out)
    if not regions:
        r.fail("mpu reports its configuration", out.replace("\n", " | ")[:90])
        return False
    r.ok("mpu reports its configuration",
         "{} regions, {}".format(
             regions.group(1),
             "packages unprivileged" if "unprivileged" in out else "packages privileged"))

    stack = re.search(r"Package stack\s*:\s*(\d+) of (\d+)", out)
    if stack:
        r.note("package stack peak {} of {} bytes".format(stack.group(1), stack.group(2)))
    fault = re.search(r"Fault handler\s*:\s*(\d+) of (\d+)", out)
    if not fault:
        r.note("no fault-handler figure in this build")
        return True
    used = int(fault.group(1))
    return r.check(used == 0, "nothing faulted during this pass",
                   "fault handler stack used {} bytes".format(used))


def main():
    ap = argparse.ArgumentParser(
        description="Smoke-test an RPCortex v2 board over its serial console.")
    ap.add_argument("--port", default="/dev/ttyACM0",
                    help="serial device (default: /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-reboot", action="store_true",
                    help="use the shell that is already there instead of "
                         "restarting the board first")
    ap.add_argument("--greet", default="/greet.app",
                    help="a package file on the DEVICE to install (default: "
                         "/greet.app)")
    args = ap.parse_args()

    print("RPCortex v2 device smoke test — {} at {} baud".format(args.port, args.baud))
    b = Board(args.port, args.baud)
    b.open()
    r = Report()

    if args.no_reboot:
        b.wake()
        r.note("started from the running shell; the boot itself is unchecked")
    else:
        print("  ...   rebooting")
        if not r.check(b.reboot(), "the board boots to a shell", "",
                       fail_detail="no prompt came back after a reboot"):
            print("\n  {} passed, {} failed".format(r.passed, r.failed))
            return 1

    check_version(b, r)
    check_install_and_run(b, r, args.greet, "greet", "hello from the greet package")
    check_novad1_service(b, r)
    check_shot(b, r)
    check_meminfo(b, r)
    check_mpu(b, r)

    b.close()
    print("\n  {} passed, {} failed".format(r.passed, r.failed))
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
