#!/usr/bin/env python3
"""The package catalogue an image ships with: the PUBLISHED index, minified.

    tools_stock_index.py <package repo checkout> <fallback index.json> <out.json>

The catalogue is written to /os/pkg/repo.json on a first boot, and `pkg install`
checks every download against the SHA-256 it lists. So it must describe what the
repository actually serves. Taken from the working copy of a package repository
checked out beside this one, it once listed packages committed there but not yet
pushed: v2.0.640-beta to v2.0.664-beta shipped Nova D1 1.8.0 while 1.7.1 was what
could be downloaded, and a fresh board refused the install with a checksum
mismatch.

So: the index as committed on the checkout's origin/main (what was published,
as of its last fetch), and the tracked copy in this repository when there is no
checkout. Minified, because whitespace is a sixth of it and flash is scarce on
the Pico (the device's parser does not care about layout).
"""
import json
import os
import subprocess
import sys


def published(repo):
    if not os.path.isdir(os.path.join(repo, ".git")):
        return None
    try:
        out = subprocess.run(["git", "-C", repo, "show", "origin/main:repo-v2/index.json"],
                             capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout if out.returncode == 0 and out.stdout else None


def main():
    repo, fallback, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    raw, where = published(repo), "origin/main of " + os.path.basename(os.path.abspath(repo))
    if raw is None:
        with open(fallback, "rb") as f:
            raw = f.read()
        where = os.path.basename(fallback)
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("packages"), list) or not data["packages"]:
        raise SystemExit("stock index from %s has no packages" % where)
    out = json.dumps(data, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    # Written only when it changed, so an unchanged catalogue rebuilds nothing.
    old = None
    if os.path.exists(dst):
        with open(dst, "rb") as f:
            old = f.read()
    if old != out:
        with open(dst, "wb") as f:
            f.write(out)
    print("stock index: %d packages from %s, %d bytes (%d before minifying)"
          % (len(data["packages"]), where, len(out), len(raw)))


if __name__ == "__main__":
    main()
