#!/usr/bin/env python3
"""Say when a board's firmware image grew more than it should have.

build.sh prints how much of the update slot is left, and that number only means
something to somebody who remembers what it was last time. A change that looked
like a few lines once cost 16 KB, because the board is built at -O3 and the
compiler unrolled a small loop into every function that called it; it showed up
as "149 KB left" where the build before had said 165, and nothing pointed at it.

So each board's image size is kept in tools/size-baseline.json, which is
committed, and every build compares against it. Growth past the threshold is
said out loud, with the way to look into it. It never fails the build: an image
is allowed to grow, as long as somebody decided it should.

    size-guard.py check  <board> <image.bin>     print a line if it moved a lot
    size-guard.py update <board> <image.bin>     record this size as the baseline
"""
import json
import os
import sys

THRESHOLD = 8 * 1024        # bytes; a normal feature is well under this


def baseline_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "size-baseline.json")


def load():
    try:
        with open(baseline_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("check", "update"):
        print(__doc__.strip().splitlines()[-2])
        print(__doc__.strip().splitlines()[-1])
        return 2
    mode, board, image = sys.argv[1:]
    size = os.path.getsize(image)
    base = load()

    if mode == "update":
        base[board] = size
        with open(baseline_path(), "w") as f:
            json.dump(base, f, indent=2, sort_keys=True)
            f.write("\n")
        print("    baseline for %s is now %d KB" % (board, size // 1024))
        return 0

    if board not in base:
        print("    no size baseline for %s yet: tools/size-guard.py update %s %s" % (board, board, image))
        return 0
    delta = size - base[board]
    if delta > THRESHOLD:
        print("    [?] %s grew %d KB since the recorded baseline (%d KB -> %d KB)."
              % (board, delta // 1024, base[board] // 1024, size // 1024))
        print("        tools/size-report.py shows where; if the growth is wanted,")
        print("        tools/size-guard.py update %s %s records it." % (board, image))
    elif delta < -THRESHOLD:
        print("    %s is %d KB smaller than its baseline; 'update' records the saving."
              % (board, -delta // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
