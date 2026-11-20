#!/usr/bin/env python3
"""Measure a board running RPCortex v2, so two firmwares can be compared.

    tools/benchmark.py run  [--port P] [--ip ADDR] [--wifi SSID] --out result.json
    tools/benchmark.py compare before.json after.json      a table with the change in percent

`run` measures the board it finds, whatever firmware it runs. It works through
link mode where the firmware has it and through the prompt where it does not
(v2.00-beta), using tools/rpc. The board should have a saved Wi-Fi network,
and the computer should be on the same network for the ping test.

What it measures:

  speed        boot time (from the board's own log), free memory and the largest
               free block after boot, bytes the console sends back per key,
               ping (average and worst, from this computer), an HTTPS request,
               upload speed, a package install, and the CPU score of the
               `bench` package
  reliability  a set of scenarios that each misbehaved in some release; each
               passes or fails, and a scenario that takes the board down is a
               failure the run recovers from

Destructive on purpose, within limits: it removes and reinstalls packages,
writes in /tmp, and removes /usb (which a correct firmware refuses, and which
is put back). Run it on a test board.
"""
import argparse
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "rpc"))

import serial                                         # noqa: E402
from rpctool import device as _device                 # noqa: E402
from rpctool import transport as _transport           # noqa: E402

ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def log(msg):
    print(msg, flush=True)


def connect(port, wait=60):
    """A Device, waiting for the board to come back if it is restarting."""
    end = time.time() + wait
    last = None
    while time.time() < end:
        try:
            return _device.Device.open(port)
        except Exception as e:                       # the port is coming and going
            last = e
            time.sleep(1.0)
    raise SystemExit("the board did not come back: %s" % last)


def run_cmd(port, dev, line, timeout=60):
    """exec, reconnecting once if the board went away. Returns (dev, rc, text, ok)."""
    try:
        r = dev.exec(line, timeout=timeout)
        return dev, r.rc, r.text, True
    except Exception as e:
        log("    (board stopped answering during %r: %s)" % (line, type(e).__name__))
        try:
            dev.close()
        except Exception:
            pass
        time.sleep(3)
        return connect(port, 90), 1, "", False


# --- measurements -----------------------------------------------------------

def echo_bytes(port):
    """Bytes the console sends back while a 48-character line is typed."""
    s = serial.Serial(_transport.choose_port(port), 115200, timeout=0.05)
    s.dtr = True
    s.write(b"\x03"); time.sleep(0.4); s.reset_input_buffer()
    cmd = "echo the quick brown fox jumps over the lazy dog"
    n = 0
    for ch in cmd:
        s.write(ch.encode()); s.flush(); time.sleep(0.02)
        n += len(s.read(65536))
    time.sleep(0.3); n += len(s.read(65536))
    s.write(b"\x15\x03"); time.sleep(0.3); s.read(65536)
    s.close()
    return n / len(cmd)


def console_line(port, line, wait=1.5):
    """Type one line at the plain prompt and return what came back."""
    s = serial.Serial(_transport.choose_port(port), 115200, timeout=0.2)
    s.dtr = True
    s.write(b"\x03"); time.sleep(0.5); s.read(65536)
    s.write(line.encode() + b"\r"); time.sleep(wait)
    out = s.read(65536)
    s.close()
    return ANSI.sub(b"", out).decode(errors="replace")


def boot_seconds(text):
    """The board's own timestamp for 'Filesystem mounted' in its latest boot."""
    blocks = re.split(r"boot #\d+", text)
    last = blocks[-1] if blocks else text
    m = re.search(r"^\s*([0-9]+\.[0-9]+)\s.*Filesystem mounted", last, re.M)
    return float(m.group(1)) if m else None


def meminfo(text):
    free = re.search(r"Free\s*:\s*(\d+) KB", text)
    big = re.search(r"Largest block\s*:\s*(\d+) KB", text)
    return (int(free.group(1)) if free else None, int(big.group(1)) if big else None)


def ping(ip, count=40):
    out = subprocess.run(["ping", "-c", str(count), "-i", "0.25", "-q", ip],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode()
    m = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)/", out)
    loss = re.search(r"(\d+)% packet loss", out)
    if not m:
        return None
    return {"min": float(m.group(1)), "avg": float(m.group(2)), "max": float(m.group(3)),
            "loss_pct": int(loss.group(1)) if loss else None}


def device_ip(text):
    m = re.search(r"(?:IP|Address)\s*:?\s*(\d+\.\d+\.\d+\.\d+)", text)
    return m.group(1) if m else None


# --- reliability scenarios --------------------------------------------------

LOOP_RPS = "set i 0\nwhile lt $i 20\necho x >> /tmp/o.txt\ninc i\nend\necho done > /tmp/done.txt\n"
CHAIN_RPS = "echo x > /tmp/c1.txt ; echo y >> /tmp/c1.txt\n"
LONG = "y" * 45      # past the 40 characters that used to be dropped, short enough for a prompt


def scenarios(port, dev):
    results = []

    def record(name, ok, note=""):
        results.append({"name": name, "pass": bool(ok), "note": note})
        log("    %-32s %s %s" % (name, "pass" if ok else "FAIL", note))

    dev.put(CHAIN_RPS.encode(), "/bench_chain.rps")
    dev.put(LOOP_RPS.encode(), "/bench_loop.rps")

    dev, rc, t, alive = run_cmd(port, dev, "script /bench_chain.rps ; cat /tmp/c1.txt")
    record("';' inside a script", alive and t.split()[-2:] == ["x", "y"], t.strip()[-40:])

    dev, rc, t, alive = run_cmd(port, dev, "wc -l /bench_chain.rps")
    record("wc -l", alive and t.strip().isdigit(), t.strip()[:40])

    dev, rc, t, alive = run_cmd(port, dev, "rm /tmp/o.txt ; bg script /bench_loop.rps")
    time.sleep(6)
    dev, rc, t, alive = run_cmd(port, dev, "wc /tmp/o.txt ; cat /tmp/done.txt")
    record("a looping script under bg", alive and "20 lines" in t and "done" in t,
           "board restarted" if not alive else t.strip().replace("\n", " ")[:50])

    dev, rc, t, alive = run_cmd(port, dev, "greet | wc -c")
    n = re.findall(r"\d+", t)
    record("package output through a pipe", alive and n and int(n[-1]) > 10, t.strip()[:40])

    dev, rc, t, alive = run_cmd(port, dev, "touch /tmp/%s" % LONG)
    dev, rc, t, alive = run_cmd(port, dev, "ls /tmp")
    ok = alive and LONG in t
    dev, rc2, t2, alive2 = run_cmd(port, dev, "ls /")
    ok = ok and alive2 and "tmp/" in t2
    record("a 45-character file name", ok, "" if ok else "the name was lost or /tmp replaced")

    dev, rc, t, alive = run_cmd(port, dev, "rm /usb")
    dev, rc2, t2, alive2 = run_cmd(port, dev, "ls /")
    kept = alive2 and "usb/" in t2
    record("rm of a system folder refused", alive and kept, "" if kept else "/usb was removed")
    if not kept:
        dev, *_ = run_cmd(port, dev, "mkdir /usb")

    dev, rc, t, alive = run_cmd(port, dev, "wifi off")
    dev, rc, t, alive = run_cmd(port, dev, "compat -q", timeout=40)
    record("compat with the radio off", alive and "tls" in t.lower(),
           "board restarted" if not alive else "")

    run_cmd(port, dev, "rm /bench_chain.rps ; rm /bench_loop.rps")
    return dev, results


# --- the run ----------------------------------------------------------------

def cmd_run(a):
    res = {"when": time.strftime("%Y-%m-%d %H:%M"), "measures": {}, "scenarios": []}
    M = res["measures"]
    port = a.port

    log("echo")
    M["echo_bytes_per_key"] = round(echo_bytes(port), 1)

    dev = connect(port)
    res["mode"] = dev.mode
    dev, rc, t, _ = run_cmd(port, dev, "ver")
    res["version"] = t.strip().splitlines()[0] if t.strip() else "?"
    log("firmware: %s (%s)" % (res["version"], dev.mode))

    log("clean state: no packages, then a restart")
    dev, rc, t, _ = run_cmd(port, dev, "pkg list")
    for name in re.findall(r"^\s+(\S+)\s+\d", t, re.M):
        run_cmd(port, dev, "pkg remove " + name)
    try:
        dev.reboot()
    except Exception:
        pass
    try:
        dev.close()
    except Exception:
        pass
    time.sleep(4)
    t0 = time.time()
    dev = connect(port, 90)
    M["restart_to_ready_s"] = round(time.time() - t0 + 4, 1)
    dev, rc, t, _ = run_cmd(port, dev, "uptime")
    up = re.search(r"(\d+)m\s*(\d+)s|(\d+)s", t)
    secs = (int(up.group(1)) * 60 + int(up.group(2))) if up and up.group(1) else (int(up.group(3)) if up else None)
    res["uptime_at_measure_s"] = secs
    if secs is None or secs > 60:
        log("    the restart did not happen (uptime %s); memory figures would not be after a boot" % secs)
    dev, rc, t, _ = run_cmd(port, dev, "log")
    M["boot_s"] = boot_seconds(t)
    # Memory is read at the plain prompt, never through link mode: an EXEC
    # holds its own output buffer (up to 32 KB) while the command runs, so a
    # meminfo inside one reports the tool's footprint as if it were the OS's.
    dev.close()
    M["free_kb"], M["largest_block_kb"] = meminfo(console_line(port, "meminfo"))
    dev = connect(port)
    log("boot %s s, free %s KB, largest %s KB" % (M["boot_s"], M["free_kb"], M["largest_block_kb"]))

    log("network")
    dev, rc, t, _ = run_cmd(port, dev, "wifi connect " + a.wifi, timeout=60)
    dev, rc, t, _ = run_cmd(port, dev, "ntp sync", timeout=30)
    dev, rc, t, _ = run_cmd(port, dev, "wifi status")
    ip = a.ip or device_ip(t)
    if ip:
        time.sleep(5)
        ping(ip, 60)        # 15 s of traffic first: the start of a new association is slow on any firmware
        p = ping(ip)
        if p:
            M["ping_avg_ms"], M["ping_max_ms"], M["ping_loss_pct"] = p["avg"], p["max"], p["loss_pct"]
            log("ping avg %.1f ms max %.1f ms" % (p["avg"], p["max"]))
    times = []
    for _ in range(3):
        s = time.time()
        dev, rc, t, _ = run_cmd(port, dev, "curl -s -I https://github.com", timeout=60)
        if rc == 0:
            times.append(time.time() - s)
    if times:
        M["https_request_s"] = round(statistics.median(times), 2)

    log("upload")
    data = os.urandom(64 * 1024)
    s = time.time()
    dev.put(data, "/tmp/bench.bin")
    M["upload_kbps"] = round(64 / (time.time() - s), 1)
    run_cmd(port, dev, "rm /tmp/bench.bin")

    log("package install from the repository")
    s = time.time()
    dev, rc, t, _ = run_cmd(port, dev, "pkg install " + a.repo_package, timeout=300)
    if rc == 0 and "nstalled" in t:
        M["package_install_s"] = round(time.time() - s, 1)
    else:
        log("    install failed: %s" % t.strip()[-120:])

    log("cpu")
    dev, rc, t, _ = run_cmd(port, dev, "pkg install bench", timeout=120)
    runs = {}
    for _ in range(3):                      # the median of three, not one run
        dev, rc, t, _ = run_cmd(port, dev, "bench", timeout=240)
        m = re.search(r"TOTAL\s+([\d,]+)", t)
        if m:
            runs.setdefault("cpu_score", []).append(int(m.group(1).replace(",", "")))
        for part in ("integer", "memory", "function", "string", "filesys"):
            mm = re.search(r"^\s*%s\s+\d+ ms\s+([\d,]+)" % part, t, re.M)
            if mm:
                runs.setdefault("bench_" + part, []).append(int(mm.group(1).replace(",", "")))
    for k, v in runs.items():
        M[k] = int(statistics.median(v))
    res["bench_runs"] = runs
    res["bench_output"] = t[-600:]

    log("scenarios")
    dev, rc, t, _ = run_cmd(port, dev, "pkg install greet", timeout=120)
    dev, res["scenarios"] = scenarios(port, dev)
    dev, rc, t, _ = run_cmd(port, dev, "wifi connect " + a.wifi, timeout=60)
    try:
        dev.close()
    except Exception:
        pass

    with open(a.out, "w") as f:
        json.dump(res, f, indent=2)
    log("written to %s" % a.out)


# --- comparing ----------------------------------------------------------------

# name, label, unit, better ("low" or "high")
ROWS = [
    ("boot_s", "Boot to filesystem mounted", "s", "low"),
    ("restart_to_ready_s", "Restart until the console answers", "s", "low"),
    ("free_kb", "Free memory after boot", "KB", "high"),
    ("largest_block_kb", "Largest free block after boot", "KB", "high"),
    ("echo_bytes_per_key", "Console bytes sent back per key", "B", "low"),
    ("ping_avg_ms", "Ping, average", "ms", "low"),
    ("ping_max_ms", "Ping, worst", "ms", "low"),
    ("https_request_s", "HTTPS request", "s", "low"),
    ("upload_kbps", "Upload to the board", "KB/s", "high"),
    ("package_install_s", "Install Nova D1 from the repository", "s", "low"),
    ("cpu_score", "CPU (bench total)", "", "high"),
    ("bench_filesys", "Filesystem (bench)", "", "high"),
]


def pct(old, new, better):
    """Improvement in percent: positive is better, whichever way 'better' runs."""
    if old in (None, 0) or new is None:
        return None
    if better == "high":
        return (new - old) / old * 100.0
    return (old - new) / old * 100.0


def cmd_compare(a):
    old = json.load(open(a.before))
    new = json.load(open(a.after))
    print("Before: %s\nAfter:  %s\n" % (old.get("version"), new.get("version")))
    print("| Measure | Before | After | Change |")
    print("|---|---|---|---|")
    for key, label, unit, better in ROWS:
        o, n = old["measures"].get(key), new["measures"].get(key)
        if o is None and n is None:
            continue
        p = pct(o, n, better)
        ch = "" if p is None else ("%+.0f%% %s" % (p, "better" if p >= 0 else "worse"))
        if p is not None and better == "low" and n and o and o / n >= 2:
            ch += " (%.0fx faster)" % (o / n) if unit in ("s", "ms") else ""
        fmt = lambda v: "n/a" if v is None else ("%s %s" % (v, unit)).strip()
        print("| %s | %s | %s | %s |" % (label, fmt(o), fmt(n), ch))
    so = old.get("scenarios", []); sn = new.get("scenarios", [])
    po = sum(1 for s in so if s["pass"]); pn = sum(1 for s in sn if s["pass"])
    print("\nReliability scenarios: %d of %d before, %d of %d after.\n" % (po, len(so), pn, len(sn)))
    names = [s["name"] for s in sn] or [s["name"] for s in so]
    print("| Scenario | Before | After |")
    print("|---|---|---|")
    for nm in names:
        b = next((s for s in so if s["name"] == nm), None)
        c = next((s for s in sn if s["name"] == nm), None)
        show = lambda s: "n/a" if s is None else ("pass" if s["pass"] else "fail" + (" (%s)" % s["note"] if s["note"] else ""))
        print("| %s | %s | %s |" % (nm, show(b), show(c)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--port")
    r.add_argument("--ip", help="the board's address, if wifi status does not show it")
    r.add_argument("--wifi", default="", help="a saved network to join")
    r.add_argument("--repo-package", default="novad1",
                   help="a package to time installing from the repository (download included)")
    r.add_argument("--out", required=True)
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    a = ap.parse_args()
    (cmd_run if a.cmd == "run" else cmd_compare)(a)


if __name__ == "__main__":
    main()
