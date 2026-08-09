#!/usr/bin/env python3
"""Rebuild releases/latest.json from the images in out/.

Run after ./build.sh. The manifest carries the size and SHA-256 of every image,
and a device refuses to flash anything whose hash does not match — so a manifest
that has drifted from the binaries is a failed update rather than a bad one.
"""
import hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/"
BOARDS = ("pico2_w", "pico_w", "pico2", "pico")

# Frozen until v2.0.0 actually ships - see os/kernel/kernel.h for why. What
# moves between beta builds is the fourth component below.
BASE_VER = "2.0.0"


def build_number():
    """The commit count, which is what the firmware bakes in as its build.

    Derived rather than passed so the manifest cannot claim a build the images
    were not built at - a manifest that disagrees with the binary is a failed
    update, not a wrong label.
    """
    try:
        n = subprocess.check_output(["git", "-C", ROOT, "rev-list", "--count", "HEAD"],
                                    stderr=subprocess.DEVNULL).decode().strip()
        return n or "0"
    except Exception:
        return "0"


def main():
    ver = sys.argv[1] if len(sys.argv) > 1 else "%s.%s" % (BASE_VER, build_number())
    bits = ver.split(".")
    shown, build = ".".join(bits[:3]), (bits[3] if len(bits) > 3 else "")
    out = []
    for b in BOARDS:
        p = os.path.join(ROOT, "out", "rpcortex-v2-%s.bin" % b)
        if not os.path.exists(p):
            print("  skip %s (not built)" % b)
            continue
        data = open(p, "rb").read()
        out.append({
            "name": b,
            "ver": ver,
            "desc": "RPCortex v%s Vela II for %s%s"
                    % (shown, b, " build %s" % build if build else ""),
            "author": "dash1101",
            "arch": "armv6m" if b in ("pico", "pico_w") else "armv8m",
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "url": BASE + "rpcortex-v2-%s.bin" % b,
        })
        print("  %-8s %6d KB  %s" % (b, len(data) // 1024, out[-1]["sha256"][:16]))
    path = os.path.join(ROOT, "releases", "latest.json")
    json.dump({"name": "RPCortex releases", "format": 2,
               "maintainer": "dash1101", "packages": out},
              open(path, "w"), indent=2)
    print("wrote %s - %d board(s)" % (path, len(out)))


main()
