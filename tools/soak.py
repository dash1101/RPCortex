#!/usr/bin/env python3
"""Leave a board running and write down what it does.

    tools/soak.py                          # /dev/ttyACM0, 72 hours, soak.log
    tools/soak.py --hours 8 --log run.log --ssid mynet
    tools/soak.py --interval 120 --check-every 1800

One line per sample, flushed as it is written, so the log survives whatever
happens to the host. A sample is `uptime`, `meminfo`, `df`, `ps` and the tail of
`logdump`; once an hour (--check-every) it also makes an HTTPS request, which
needs about 45 KB of free heap and is therefore the first thing a slow leak
breaks.

WHAT IT LOOKS FOR
-----------------
  * RESET        uptime went backwards. The log and any crash report are
                 copied into the soak log at once, before the next reset can
                 push them out of the ring.
  * FS FULL      the filesystem reads 0 KB free. Nothing is deleted and nothing
                 is written: the state is the evidence (see PAR-63).
  * NO HTTPS     the hourly request failed. The free heap at that moment is
                 logged beside it.
  * LOGIN        the board is asking for a password. The soak stops; it does not
                 hold one and will not guess.
  * END          the run finished, or was stopped early; the line says which.
  * NEW [!]      a new warning or error line in the log ring.

It does not cover what soak.py cannot see from the console: a package that is
slowly wrong rather than slowly leaking, flash wear, or anything needing
hardware attached.

REQUIREMENTS
------------
pyserial, and a board that boots to a shell with no login prompt. The serial
port is held for the whole run, so nothing else can use it meanwhile.
"""

import argparse
import os
import re
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devsmoke  # noqa: E402  (Board, PROMPT, ANSI)

try:
    import serial
except ImportError:
    sys.exit("soak needs pyserial: pip install pyserial")


class Slow(devsmoke.Board):
    """The same console, typed one character at a time.

    A whole line arriving in one burst right after a boot was seen to come in
    reordered once, so every command here is typed.
    """

    def send(self, cmd, wait=20.0, settle=0.8):
        self.ser.reset_input_buffer()
        for ch in cmd:
            self.ser.write(ch.encode())
            self.ser.flush()
            time.sleep(0.01)
        self.ser.write(b"\r")
        self.ser.flush()
        buf = bytearray()
        start = last = time.time()
        while time.time() - start < wait:
            chunk = self.ser.read(4096)
            if chunk:
                buf.extend(chunk)
                last = time.time()
                continue
            clean = devsmoke.ANSI.sub(b"", bytes(buf))
            if devsmoke.PROMPT.search(clean.split(b"\n")[-1]) and time.time() - last >= settle:
                break
        text = devsmoke.ANSI.sub(b"", bytes(buf)).decode("utf-8", "replace")
        m = re.search(re.escape(cmd) + r"\r?\n", text)
        if m:
            text = text[m.end():]
        lines = [ln.rstrip("\r") for ln in text.split("\n")]
        while lines and (not lines[-1].strip() or devsmoke.PROMPT.search(lines[-1].encode())):
            lines.pop()
        return "\n".join(ln for ln in lines if ln.strip()).strip()


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


class Log:
    def __init__(self, path):
        self.f = open(path, "a", buffering=1)

    def line(self, text):
        self.f.write("{}  {}\n".format(stamp(), text))
        self.f.flush()
        os.fsync(self.f.fileno())

    def block(self, title, body):
        self.line(title)
        for ln in body.splitlines():
            self.f.write("    " + ln + "\n")
        self.f.flush()
        os.fsync(self.f.fileno())


def parse_uptime(text):
    m = re.search(r"Uptime:\s*(?:(\d+)d\s*)?(?:(\d+)h\s*)?(?:(\d+)m\s*)?(\d+)s", text)
    if not m:
        return None
    d, h, mi, s = (int(x or 0) for x in m.groups())
    return ((d * 24 + h) * 60 + mi) * 60 + s


def num(pattern, text):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


class Soak:
    def __init__(self, args):
        self.a = args
        self.log = Log(args.log)
        self.board = Slow(args.port)
        self.last_up = None
        self.seen = set()
        self.full_noted = False

    def connect(self):
        self.board.close()
        self.board.open(tries=120)
        self.board.wake()

    def cmd(self, c, wait=20.0):
        """Run one command; reconnect once if the port went away under it."""
        for attempt in (0, 1):
            try:
                return self.board.send(c, wait=wait)
            except (OSError, serial.SerialException):
                if attempt:
                    raise
                time.sleep(5)
                self.connect()
        return ""

    def sample(self):
        up_t = self.cmd("uptime", 15)
        if re.search(r"(?i)password|login:|username", up_t) and "Uptime" not in up_t:
            self.log.line("LOGIN  the board is asking for a password; stopping")
            return False
        up = parse_uptime(up_t)
        mem = self.cmd("meminfo", 15)
        df = self.cmd("df", 15)
        ps = self.cmd("ps", 15)

        free = num(r"Free\s*:\s*(\d+)\s*KB", mem)
        big = num(r"Largest block\s*:\s*(\d+)\s*KB", mem)
        fsfree = num(r"Free\s*:\s*(\d+)\s*KB", df)
        fsuse = num(r"\((\d+)%\)", df)
        tasks = num(r"(\d+)\s+tasks", ps)
        cpu = num(r"CPU\s+(\d+)%", ps)

        if self.last_up is not None and up is not None and up < self.last_up:
            self.log.line("RESET  uptime {}s -> {}s".format(self.last_up, up))
            self.log.block("logdump after the reset:", self.cmd("logdump", 30))
            self.log.block("crash report:", self.cmd("cat /os/crash.log", 30)[-2500:])
        if up is not None:
            self.last_up = up

        if fsfree == 0 and not self.full_noted:
            self.full_noted = True
            self.log.line("FS FULL  df reports 0 KB free. Nothing is being removed.")
            self.log.block("df:", df)
            self.log.block("ls /os:", self.cmd("ls /os", 15))
        elif fsfree:
            self.full_noted = False

        self.log.line("up={}s heap_free={}KB largest={}KB fs_free={}KB fs_used={}% "
                      "tasks={} cpu={}%".format(up, free, big, fsfree, fsuse, tasks, cpu))

        ld = self.cmd("logdump", 30)
        for ln in ld.splitlines():
            if "[!]" in ln and ln not in self.seen:
                self.seen.add(ln)
                self.log.line("NEW [!]  " + ln.strip())
        return True

    def https_check(self):
        status = self.cmd("wifi status", 15)
        if re.search(r"(?i)radio\s*:\s*off|not connected|disconnected", status):
            if not self.a.ssid:
                self.log.line("NO WIFI  the radio is off and no --ssid was given")
                return
            out = self.cmd("wifi connect " + self.a.ssid, 60)
            if "Connected" not in out:
                self.log.line("NO WIFI  could not join '{}'".format(self.a.ssid))
                return
        mem = self.cmd("meminfo", 15)
        free = num(r"Free\s*:\s*(\d+)\s*KB", mem)
        out = self.cmd("update check", 90)
        if "Installed" in out:
            self.log.line("HTTPS ok  (heap free before: {} KB)".format(free))
        else:
            self.log.block("NO HTTPS  heap free before: {} KB".format(free), out[-800:])

    def run(self):
        # Stopped early (SIGTERM, or Ctrl+C): say so in the log rather than just stopping.
        def stop(signum, _frame):
            self.log.line("END  stopped early (signal %d)" % signum)
            sys.exit(0)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        self.connect()
        t0 = time.time()
        end = t0 + self.a.hours * 3600
        next_check = t0 + 60
        self.log.line("START  port={} interval={}s hours={} check-every={}s".format(
            self.a.port, self.a.interval, self.a.hours, self.a.check_every))
        self.log.block("version:", self.cmd("version", 15))
        while time.time() < end:
            try:
                if not self.sample():
                    return 2
                if time.time() >= next_check:
                    self.https_check()
                    next_check = time.time() + self.a.check_every
            except (OSError, serial.SerialException) as exc:
                self.log.line("PORT  {}; reconnecting".format(exc))
                try:
                    self.connect()
                except SystemExit as e:
                    self.log.line("PORT  gave up: {}".format(e))
                    return 3
            time.sleep(self.a.interval)
        self.log.line("END  {} hours elapsed".format(self.a.hours))
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--log", default="soak.log")
    ap.add_argument("--hours", type=float, default=72.0)
    ap.add_argument("--interval", type=int, default=300, help="seconds between samples")
    ap.add_argument("--check-every", type=int, default=3600,
                    help="seconds between HTTPS requests")
    ap.add_argument("--ssid", default="",
                    help="a saved network to join for the HTTPS check (none: use what is up)")
    args = ap.parse_args()
    return Soak(args).run()


if __name__ == "__main__":
    sys.exit(main())
