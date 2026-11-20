#!/usr/bin/env python3
"""Choose a board and its features, then build it.

    tools/buildconf.py

Asks which board, then goes through the features that board can have one at a time,
showing what leaving each out saves, and ends with the ./build.sh command for what was
chosen. It can run that command too. Nothing here is needed to build: it only saves
remembering the flags. ./build.sh --features lists the same things without asking.

The answers it accepts are plain: a number or a board name, then y or n (Enter keeps the
board's standard choice). Ctrl+C at any point stops without building anything.
"""

import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import features as F   # noqa: E402

BOARD_WORDS = {
    "pico2_w": "Pico 2 W  (RP2350, Wi-Fi and Bluetooth)",
    "pico_w":  "Pico W    (RP2040, Wi-Fi and Bluetooth)",
    "pico2":   "Pico 2    (RP2350)",
    "pico":    "Pico      (RP2040)",
}

# How each feature reads in a question: "Keep the add-on drivers?"
NOUN = {"wifi": "Wi-Fi", "bluetooth": "Bluetooth", "ssh": "SSH", "sd": "the microSD driver",
        "peripherals": "the add-on drivers", "devpkgs": "the development packages"}


def ask(prompt, default):
    try:
        a = input(prompt).strip()
    except EOFError:
        a = ""
    return a or default


def yes_no(prompt, default):
    while True:
        a = ask("%s [%s] " % (prompt, "Y/n" if default else "y/N"), "y" if default else "n").lower()
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False
        print("  Please answer y or n.")


def pick_board():
    print("Which board?")
    for i, b in enumerate(F.BOARDS, 1):
        print("  %d. %-8s %s" % (i, b, BOARD_WORDS[b]))
    while True:
        a = ask("Board [1]: ", "1").lower()
        if a.isdigit() and 1 <= int(a) <= len(F.BOARDS):
            return F.BOARDS[int(a) - 1]
        if a in F.BOARDS:
            return a
        print("  Type a number from the list, or a board's name.")


def estimate(sizes, board, with_, without):
    """Roughly how big the image will be, from the measured variants. Leaving out
    Wi-Fi takes Bluetooth and SSH with it, and its measurement already counts them."""
    try:
        std = sizes["boards"][board]["standard"]
    except (KeyError, TypeError):
        return None
    flash = std["flash"]
    ram = std["ram"]
    out = set(without)
    if "wifi" in out:
        out -= {"bluetooth", "ssh"}
    for name in sorted(out):
        f = F.feature(name)
        s = F.saving(sizes, board, "no" + f.tag)
        if not s:
            return None
        flash -= s[0]
        ram -= s[1]
    for name in with_:
        f = F.feature(name)
        s = F.saving(sizes, board, f.tag)
        if not s:
            return None
        flash -= s[0]
        ram -= s[1]
    return flash, ram, std.get("slot")


def main():
    print("RPCortex v2: build an image with the features you want.")
    print("Every board's standard image already has everything it can have, so most")
    print("people never need this. It is for leaving things out, to make room or to")
    print("build a board with no radio at all.")
    print()
    board = pick_board()
    sizes = F.load_sizes()
    std = F.standard(board)
    print()
    print("The standard %s image has: %s." % (board, ", ".join(
        F.feature(n).title for n in std) or "none of the optional parts"))
    print("For each one, Enter keeps the standard choice.")

    with_, without, kept = [], [], []
    for f in F.FEATURES:
        if board not in f.boards:
            continue
        if any(r not in kept for r in f.requires):
            continue            # what it needs is already left out
        print()
        print("  %s" % f.title)
        for line in F.wrap(f.what, 72):
            print("    " + line)
        on_by_default = f.name in std
        if on_by_default:
            s = F.saving(sizes, board, "no" + f.tag)
            if s:
                print("    Leaving it out saves %s of flash%s." % (
                    F.kb(s[0]), (" and %s of RAM" % F.kb(s[1])) if s[1] >= 512 else ""))
            keep = yes_no("    Keep %s?" % NOUN.get(f.name, f.title), True)
            if keep:
                kept.append(f.name)
            else:
                without.append(f.name)
        else:
            s = F.saving(sizes, board, f.tag)
            if s:
                print("    Adding it costs %s of flash." % F.kb(-s[0]))
            if yes_no("    Add %s?" % NOUN.get(f.name, f.title), False):
                with_.append(f.name)
                kept.append(f.name)

    chosen, notes = F.resolve(board, with_, without)
    tag = F.variant(board, chosen)
    cmd = ["./build.sh", board]
    if with_:
        cmd += ["--with", ",".join(with_)]
    if without:
        cmd += ["--without", ",".join(without)]

    print()
    if tag:
        print("Your image: a custom %s image (%s)." % (board, tag))
        print("  It builds to out/rpcortex-v2-%s-%s.uf2, and `ver` on the board will say" % (board, tag))
        print("  it is custom. A release never publishes it, and `update install` would")
        print("  replace it with the standard image.")
    else:
        print("Your image: the standard %s image, out/rpcortex-v2-%s.uf2." % (board, board))
    for n in notes:
        print("  note: " + n)
    est = estimate(sizes, board, with_, without)
    too_big = False
    if est:
        flash, ram, slot = est
        too_big = bool(slot) and flash > slot
        if too_big:
            print("  About %s of flash, which is more than the %s firmware slot. build.sh"
                  % (F.kb(flash), F.kb(slot)))
            print("  will refuse it: an image that big could not be installed by an update.")
            print("  Leave something else out to make room.")
        else:
            line = "  About %s of flash" % F.kb(flash)
            if slot:
                line += ", leaving about %s of the %s firmware slot" % (F.kb(slot - flash), F.kb(slot))
            print(line + ".")
        print("  (Estimated from %s; the build prints the real figure.)" % sizes.get("measured_at", "earlier builds"))
    print()
    print("The command:")
    print("  " + " ".join(shlex.quote(c) for c in cmd))
    print()
    if too_big:
        print("Not built: it would not fit.")
        return 1
    if yes_no("Build it now?", False):
        print()
        return subprocess.call(cmd, cwd=ROOT)
    print("Not built. Run the command above when you want it.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped. Nothing was built.")
        sys.exit(130)
