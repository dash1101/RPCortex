#!/usr/bin/env python3
"""Check every attached board before a pre-release goes out, and keep the readings.

    tools/pretest.py                      every RPCortex board on USB
    tools/pretest.py --port /dev/ttyACM0  one board
    tools/pretest.py --user dash          log in as this account where a board asks
                                          (the password from $RPC_PASSWORD)
    tools/pretest.py --out reports/       also write <version>-<board>.md there

For each board it:

  * reads what it is (sysinfo, the partition it runs from);
  * installs the selftest package from this build (os/build_<board>/apps/
    selftest.app), or from the repository when there is no build, and runs it:
    memory, the filesystem, sha256, timers, tasks, the shell, the registry,
    random numbers, and the network (DNS, HTTPS, and a certificate that has to
    be refused);
  * sends 128 KB to the board and fetches it back over the link, checksummed
    both ways, and reports the speed.

Every check prints PASS, FAIL or SKIP; the exit status is 1 when anything
failed. The readings (speeds, timings, free memory) are there to be compared
with the last pre-release's: a check can pass and still be twice as slow.

What it does not do: anything needing parts wired to the board, the 72-hour
soak (tools/soak.py), the Nova D1 screen (tools/devsmoke.py), or updating
firmware. It changes nothing on a board but the selftest package, installed or
updated, and a file in /tmp that it removes.
"""
import argparse
import datetime
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STUDIO = os.environ.get("RPC_STUDIO_DIR") or os.path.join(ROOT, "..", "rpcortex-studio")
RPC = [sys.executable, os.path.join(STUDIO, "rpc.py")]


def rpc(port, user, *args, timeout=600):
    cmd = RPC + ["-p", port] + (["-u", user] if user else []) + list(args)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def boards(explicit):
    if explicit:
        return [explicit]
    sys.path.insert(0, STUDIO)
    from rpctool import transport          # noqa: E402
    found = [p for p in transport.list_candidates() if p.rpcortex]
    # The console only: a board's second port carries the network bridge.
    out = []
    for p in found:
        iface = (p.interface or "").lower()
        if "network" in iface:
            continue
        out.append(p.device)
    return out


def one_board(port, user, out_dir):
    lines, failed = [], 0

    def say(name, ok, text):
        nonlocal failed
        word = "PASS" if ok is True else "FAIL" if ok is False else "SKIP"
        if ok is False:
            failed += 1
        line = "  {:<10} {}  {}".format(name, word, text)
        lines.append(line)
        print(line)

    rc, info = rpc(port, user, "exec", "sysinfo ; update rollback")
    ver = re.search(r"OS Version\s*:\s*(\S+)", info)
    plat = re.search(r"Platform\s*:\s*(\S+)", info)
    part = re.search(r"Running\s+.*?(partition [AB])", info)
    if not ver or not plat:
        print("{}: no answer ({})".format(port, info.strip().splitlines()[-1:] or "nothing"))
        return 1, []
    version, board = ver.group(1), plat.group(1)
    head = "{} on {} ({}{})".format(version, board, port, ", " + part.group(1) if part else "")
    print(head)
    lines.append(head)

    # selftest: this build's, else the repository's.
    app = os.path.join(ROOT, "os", "build_" + board, "apps", "selftest.app")
    if os.path.exists(app):
        rpc(port, user, "put", app, "/tmp/selftest.app")
        rc, out = rpc(port, user, "--sudo", "exec", "sudo pkg install /tmp/selftest.app ; rm /tmp/selftest.app")
        source = "this build"
    else:
        rc, out = rpc(port, user, "--sudo", "exec", "sudo pkg install selftest")
        source = "the repository"
    if "Installed selftest" not in out and "selftest" not in out:
        say("install", False, "selftest would not install from {}: {}".format(source, out.strip()[-120:]))
    rc, out = rpc(port, user, "exec", "selftest", timeout=300)
    seen = 0
    for m in re.finditer(r"^selftest\s+(\S+)\s+(PASS|FAIL|SKIP)\s+(.*)$", out, re.M):
        seen += 1
        say(m.group(1), {"PASS": True, "FAIL": False, "SKIP": None}[m.group(2)], m.group(3).strip())
    if not seen:
        say("selftest", False, "printed nothing a check could be read from: " + out.strip()[-160:])

    # The link: 128 KB there and back.
    data = os.urandom(128 * 1024)
    fd, local = tempfile.mkstemp(suffix=".bin", prefix="pretest-")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    t0 = time.monotonic()
    rc, out = rpc(port, user, "put", local, "/tmp/pretest.bin")
    t_put = time.monotonic() - t0
    back = local + ".back"
    t0 = time.monotonic()
    rc2, out2 = rpc(port, user, "get", "/tmp/pretest.bin", back)
    t_get = time.monotonic() - t0
    same = os.path.exists(back) and hashlib.sha256(open(back, "rb").read()).digest() == hashlib.sha256(data).digest()
    rpc(port, user, "exec", "rm /tmp/pretest.bin")
    for p in (local, back):
        if os.path.exists(p):
            os.remove(p)
    kb = len(data) / 1024
    say("link", rc == 0 and rc2 == 0 and same,
        "128 KB up {:.1f} KB/s, down {:.1f} KB/s, {}".format(
            kb / t_put if t_put else 0, kb / t_get if t_get else 0,
            "identical both ways" if same else "DIFFERED"))

    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        name = os.path.join(out_dir, "{}-{}.md".format(version, board))
        with open(name, "w") as f:
            f.write("# pretest {}\n\n{}\n\n```\n{}\n```\n".format(
                datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), head, "\n".join(lines[1:])))
        print("  written to " + name)
    return failed, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="one board's console, instead of every one found")
    ap.add_argument("--user", default=os.environ.get("RPC_USER"), help="account to log in as where a board asks")
    ap.add_argument("--out", help="folder to write a report per board into")
    a = ap.parse_args()
    ports = boards(a.port)
    if not ports:
        print("No RPCortex board found. Plug one in, or name it with --port.")
        return 1
    total = 0
    for p in ports:
        failed, _ = one_board(p, a.user, a.out)
        total += failed
        print()
    print("pretest: {} board(s), {}".format(len(ports), "all passed" if not total else "{} FAILED".format(total)))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
