#!/usr/bin/env python3
"""What an RPCortex v2 image can be built with or without.

    tools/features.py list                    every feature, the boards that can have it,
                                              and what leaving it out saves on each
    tools/features.py resolve BOARD [--with a,b] [--without c,d]
                                              shell assignments build.sh evaluates
    tools/features.py cmake BOARD OUT [name=ON|OFF ...]
                                              the settings os/CMakeLists.txt includes
    tools/features.py measure [BOARD ...]     build each variant and record its size in
                                              tools/feature-sizes.json (several minutes)

ONE TABLE, HERE. build.sh, os/CMakeLists.txt, tools/buildconf.py and tools/release.py all
read it, so a feature's name, its boards and its default cannot disagree between them. A
second copy in CMake would be the drift build.sh's slot check exists to catch.

THE STANDARD IMAGE for a board is every feature that board can have, except SSH on the Pico W
(its slot is too tight) and the development packages. That is what a release publishes. Anything else is a custom image: its file name
says how it differs (rpcortex-v2-pico2_w-nobt.uf2), `ver` on the device says so, and
tools/release.py refuses to publish it.

THE FLASH LAYOUT DOES NOT CHANGE with the features. The firmware slot, the package slot and
the filesystem start where they always do for that board, so moving between a standard and
a custom image keeps every file. What a custom image saves is slot headroom and RAM, not
filesystem.

Only the standard library is needed.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIZES = os.path.join(HERE, "feature-sizes.json")

BOARDS = ("pico2_w", "pico_w", "pico2", "pico")
CHIP = {"pico2_w": "RP2350", "pico2": "RP2350", "pico_w": "RP2040", "pico": "RP2040"}
RADIO = ("pico2_w", "pico_w")          # the boards with the CYW43 Wi-Fi and Bluetooth chip
RP2350 = ("pico2_w", "pico2")


class Feature:
    def __init__(self, name, tag, cmake, define, title, what, boards, default=True,
                 requires=(), aliases=(), note=""):
        self.name = name          # what --with/--without take
        self.tag = tag            # what an image name carries: -no<tag>, or -<tag> when added
        self.cmake = cmake        # the CMake option
        self.define = define      # the compile definition the source tests (always 0 or 1)
        self.title = title        # for people
        self.what = what
        self.boards = boards      # the boards that can have it at all
        self.default = default    # True: on wherever the board can have it (and its requirements are on)
        self.requires = requires
        self.aliases = aliases
        self.note = note


# In dependency order: a feature comes after everything it requires.
FEATURES = (
    Feature("wifi", "wifi", "RPC_FEATURE_WIFI", "RPC_HAS_WIFI", "Wi-Fi",
            "Wi-Fi and everything that needs a network: lwIP, HTTPS (mbedTLS) and its trusted "
            "roots; wifi, net, ping, nslookup, ntp, wget, curl and runurl; pkg install, update "
            "and upgrade from the repository; update check and install over the air; and the "
            "network calls packages make (fw_net_*, fw_http_*, fw_tcp_*).",
            RADIO, aliases=("wi-fi", "network", "net"),
            note="Left out, Bluetooth and SSH go with it, and so do the CYW43 driver and its firmware."),
    Feature("bluetooth", "bt", "RPC_FEATURE_BLUETOOTH", "RPC_HAS_BLUETOOTH", "Bluetooth",
            "btstack (LE and Classic) and the bt and btaudio commands. It runs on the "
            "radio chip's driver, which comes with Wi-Fi.",
            RADIO, requires=("wifi",), aliases=("bt",)),
    # The CMake option is RPC_SSH, the one the SSH work introduced; it is driven
    # from here like the others. Off by default on the Pico W, whose slot has too
    # little left for it, and it runs on Wi-Fi, so it goes when Wi-Fi does.
    Feature("ssh", "ssh", "RPC_SSH", "RPC_HAS_SSH", "SSH",
            "The SSH server (`ssh server`) and client (`ssh`), with the key exchange, host "
            "key and ciphers they need on top of mbedTLS.",
            RADIO, requires=("wifi",),
            default=lambda board: CHIP[board] == "RP2350"),
    Feature("sd", "sd", "RPC_FEATURE_SD", "RPC_HAS_SD", "microSD",
            "The SPI microSD driver, its FAT reader, the sd command and the /sd root packages "
            "can browse.",
            RP2350, aliases=("microsd", "sdcard")),
    Feature("peripherals", "periph", "RPC_FEATURE_PERIPHERALS", "RPC_HAS_PERIPHERALS",
            "Add-on drivers",
            "Drivers for add-on boards: NFC (PN532), iButton (1-Wire), sub-GHz (CC1101) and "
            "LoRa (SX1276), with the nfc, ibutton, subghz and lora commands and the NFC calls "
            "packages make (fw_nfc_*).",
            BOARDS, aliases=("periph", "drivers", "addons")),
    Feature("devpkgs", "dev", "RPC_DEV_PACKAGES", "RPC_DEV_PACKAGES", "Development packages",
            "bench, probe, stress and havoc, baked into the image so a board can be tested "
            "before it has a network. For working on the OS; a release never has them.",
            BOARDS, default=False, aliases=("dev", "dev-packages", "devpackages")),
)

BY_NAME = {}
for _f in FEATURES:
    for _n in (_f.name,) + _f.aliases:
        BY_NAME[_n] = _f


class FeatureError(Exception):
    pass


def feature(name):
    f = BY_NAME.get(name.strip().lower())
    if not f:
        raise FeatureError("no feature called '%s'. The features are: %s"
                           % (name, ", ".join(x.name for x in FEATURES)))
    return f


def split_names(text):
    """'wifi,bt' or 'wifi bt' or a list of either, into feature names. Empty parts are ignored."""
    if text is None:
        return []
    if isinstance(text, (list, tuple)):
        out = []
        for t in text:
            out += split_names(t)
        return out
    return [p for p in text.replace(",", " ").split() if p]


def check_board(board):
    if board not in BOARDS:
        raise FeatureError("no board called '%s'. The boards are: %s" % (board, ", ".join(BOARDS)))


def default_on(f, board, chosen):
    """Whether f is in the board's image when nobody asks, given what is already chosen."""
    if board not in f.boards:
        return False
    if any(r not in chosen for r in f.requires):
        return False
    return f.default(board) if callable(f.default) else bool(f.default)


def standard(board):
    """The standard image's features for a board, in table order."""
    check_board(board)
    chosen = []
    for f in FEATURES:
        if default_on(f, board, chosen):
            chosen.append(f.name)
    return chosen


def resolve(board, with_=(), without=(), strict=True):
    """The features an image gets, as (names in table order, notes).

    strict (build.sh, buildconf): asking for something impossible is an error.
    Not strict (a CMake cache left over from another configure): a feature whose
    requirement is off is switched off, with a note, rather than failing.
    """
    check_board(board)
    want = {feature(n).name for n in split_names(with_)}
    drop = {feature(n).name for n in split_names(without)}
    both = want & drop
    if both:
        raise FeatureError("asked both with and without: %s" % ", ".join(sorted(both)))

    notes = []
    chosen = []
    for f in FEATURES:
        if f.name in want:
            if board not in f.boards:
                raise FeatureError("%s cannot be built for %s; only for %s."
                                   % (f.name, board, ", ".join(f.boards)))
            missing = [r for r in f.requires if r not in chosen]
            if missing:
                if strict:
                    raise FeatureError("%s needs %s, which this image leaves out."
                                       % (f.name, " and ".join(missing)))
                notes.append("%s is off because %s is off." % (f.name, " and ".join(missing)))
                continue
            chosen.append(f.name)
        elif f.name in drop:
            # Said to a person who asked (build.sh, buildconf). CMake is always
            # handed every option, so there an OFF the board could not have is
            # no news.
            if board not in f.boards and strict:
                notes.append("%s has no %s to leave out." % (board, f.title))
        elif default_on(f, board, chosen):
            chosen.append(f.name)
    return chosen, notes


def variant(board, chosen):
    """The name a custom image carries ("" for the standard one): -no<tag> for each feature
    left out and -<tag> for each added. A feature that went only because something it needs
    went is not named again: -nowifi already says there is no SSH."""
    std = standard(board)
    parts = []
    for f in FEATURES:
        if f.name in std and f.name not in chosen:
            if any(r not in chosen for r in f.requires):
                continue
            parts.append("no" + f.tag)
        elif f.name in chosen and f.name not in std:
            parts.append(f.tag)
    return "-".join(parts)


def cmake_args(chosen):
    return ["-D%s=%s" % (f.cmake, "ON" if f.name in chosen else "OFF") for f in FEATURES]


# The marker a built image carries (os/shell/imagefeatures.cpp): "rpcimage:" and then the
# variant, "standard" or the custom name, ending at the string's NUL.
MARK = b"rpcimage:"


def image_variant(data):
    """The variant a built image says it is, or None when it carries no marker."""
    i = data.find(MARK)
    if i < 0:
        return None
    j = data.find(b"\0", i)
    if j < 0:
        return None
    return data[i + len(MARK):j].decode("ascii", "replace")


def uf2_payload(data):
    """The image bytes inside a .uf2: 512-byte blocks, each carrying up to 476 bytes of
    payload at offset 32 with its length at offset 16. Concatenated, so a marker that
    straddles two blocks is still found."""
    out = bytearray()
    for i in range(0, len(data) - 511, 512):
        blk = data[i:i + 512]
        if blk[0:4] != b"UF2\n":
            continue
        n = int.from_bytes(blk[16:20], "little")
        out += blk[32:32 + min(n, 476)]
    return bytes(out)


def image_bytes(path):
    """A built image's bytes, unwrapped from the .uf2 container when it is one."""
    with open(path, "rb") as fh:
        data = fh.read()
    return uf2_payload(data) if path.endswith(".uf2") else data


# --- sizes ---------------------------------------------------------------------

def load_sizes(path=SIZES):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def saving(sizes, board, tag):
    """(flash bytes, static RAM bytes) a variant saves against the standard image, or None."""
    try:
        b = sizes["boards"][board]
        std, var = b["standard"], b["variants"][tag]
    except (KeyError, TypeError):
        return None
    return std["flash"] - var["flash"], std["ram"] - var["ram"]


def kb(n):
    return "%d KB" % round(n / 1024.0)


def headroom(sizes, board, tag):
    """'11 KB -> 132 KB of slot left' for a variant, or '' when the slot is not recorded.
    A variant that does not fit says so instead."""
    try:
        b = sizes["boards"][board]
        std, var = b["standard"], b["variants"][tag]
        slot = std["slot"]
    except (KeyError, TypeError):
        return ""
    if var.get("fits") is False or var["flash"] > slot:
        return "; does NOT fit the %s slot" % kb(slot)
    return "; slot headroom %s -> %s" % (kb(slot - std["flash"]), kb(slot - var["flash"]))


def describe_saving(sizes, board, f):
    """One plain phrase for what leaving out (or adding) f does on a board."""
    if f.default is False:
        s = saving(sizes, board, f.tag)
        if not s:
            return "not measured"
        return ("adds %s of flash" % kb(-s[0]) +
                (" and %s of RAM" % kb(-s[1]) if abs(s[1]) >= 512 else "") +
                headroom(sizes, board, f.tag))
    s = saving(sizes, board, "no" + f.tag)
    if not s:
        return "not measured"
    text = "saves %s of flash" % kb(s[0])
    if s[1] >= 512:
        text += " and %s of RAM" % kb(s[1])
    return text + headroom(sizes, board, "no" + f.tag)


def cmd_list(_a):
    sizes = load_sizes()
    print("Features an image can be built with or without. A board's standard image has every")
    print("feature that board can have, except SSH on the Pico W and the development packages;")
    print("that is what a release publishes. ./build.sh <board> --without a,b  or  --with a,b")
    print("builds anything else, and tools/buildconf.py asks the questions for you.")
    print()
    for f in FEATURES:
        print("  %-12s %s%s" % (f.name, f.title,
                                "  (also spelt %s)" % ", ".join(f.aliases) if f.aliases else ""))
        for line in wrap(f.what, 78 - 6):
            print("      " + line)
        std = [b for b in f.boards if f.name in standard(b)]
        extra = [b for b in f.boards if b not in std]
        if f.default is False:
            print("      Boards: any, never in a standard image.")
        else:
            print("      Boards: %s%s." % (", ".join(f.boards) if f.boards != BOARDS else "all",
                                          (" (standard on %s)" % ", ".join(std)) if extra else ""))
        if f.requires:
            print("      Needs: %s." % ", ".join(f.requires))
        for b in f.boards:
            print("      %-8s %s" % (b, describe_saving(sizes, b, f)))
        if f.name == "wifi":
            print("      With bluetooth left out too, the radio driver and its firmware go as well:")
            for b in f.boards:
                s = saving(sizes, b, "nowifi-nobt")
                if s:
                    print("      %-8s saves %s of flash and %s of RAM%s" % (
                        b, kb(s[0]), kb(s[1]), headroom(sizes, b, "nowifi-nobt")))
        print()
    if sizes:
        print("Sizes measured at %s, from the .bin and the ELF's static RAM. They do not simply" %
              sizes.get("measured_at", "an earlier build"))
        print("add up: the radio driver is shared by Wi-Fi and Bluetooth and only goes when both do.")
        print("tools/features.py measure records them again.")
    else:
        print("No sizes recorded yet: tools/features.py measure builds each variant and records them.")
    return 0


def wrap(text, width):
    words, line, out = text.split(), "", []
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            out.append(line)
            line = w
        else:
            line = (line + " " + w) if line else w
    if line:
        out.append(line)
    return out


# --- build.sh and CMake ----------------------------------------------------------

def cmd_resolve(a):
    try:
        chosen, notes = resolve(a.board, a.with_, a.without, strict=True)
    except FeatureError as e:
        sys.stderr.write("features: %s\n" % e)
        return 2
    tag = variant(a.board, chosen)
    print("FEAT_LIST=%s" % shlex.quote(" ".join(chosen)))
    print("FEAT_TAG=%s" % shlex.quote(tag))
    print("FEAT_CMAKE=%s" % shlex.quote(" ".join(cmake_args(chosen))))
    print("FEAT_NOTES=%s" % shlex.quote("\n".join(notes)))
    return 0


def cmd_cmake(a):
    """Called by os/CMakeLists.txt with whatever RPC_FEATURE_* it was given (build.sh
    always gives every one). Writes a file of set() lines for it to include."""
    with_, without = [], []
    by_cmake = {f.cmake: f for f in FEATURES}
    for kv in a.settings:
        if "=" not in kv:
            sys.stderr.write("features: expected NAME=VALUE, got '%s'\n" % kv)
            return 2
        k, v = kv.split("=", 1)
        f = by_cmake.get(k) or BY_NAME.get(k.lower())
        if not f:
            sys.stderr.write("features: no feature for '%s'\n" % k)
            return 2
        v = v.strip().upper()
        if v in ("ON", "1", "TRUE", "YES", "Y"):
            with_.append(f.name)
        elif v in ("OFF", "0", "FALSE", "NO", "N"):
            without.append(f.name)
        elif v:
            sys.stderr.write("features: %s=%s is neither ON nor OFF\n" % (k, v))
            return 2
    try:
        chosen, notes = resolve(a.board, with_, without, strict=False)
    except FeatureError as e:
        sys.stderr.write("%s\n" % e)
        return 2
    lines = ["# Written by tools/features.py when this directory was configured. Not for editing."]
    for f in FEATURES:
        lines.append("set(%s %s)" % (f.cmake, "ON" if f.name in chosen else "OFF"))
        key = f.define[len("RPC_HAS_"):] if f.define.startswith("RPC_HAS_") else f.name.upper()
        lines.append("set(RPC_CAN_%s %d)" % (key, 1 if a.board in f.boards else 0))
        lines.append("set(RPC_STD_%s %d)" % (key, 1 if f.name in standard(a.board) else 0))
    lines.append('set(RPC_FEATURE_LIST "%s")' % ";".join(chosen))
    lines.append('set(RPC_FEATURE_VARIANT "%s")' % variant(a.board, chosen))
    lines.append('set(RPC_FEATURE_NOTES "%s")' % "; ".join(notes).replace('"', "'"))
    with open(a.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return 0


# --- measuring -------------------------------------------------------------------

def measured_variants():
    """(with, without) for every variant worth measuring: each feature left out on its
    own, each optional one added, and both radios out together (they share a driver).
    """
    out = [([], [])]
    for f in FEATURES:
        out.append(([f.name], []) if f.default is False else ([], [f.name]))
    out.append(([], ["wifi", "bluetooth"]))
    return out


def image_numbers(board, tag):
    suffix = ("-" + tag) if tag else ""
    binp = os.path.join(ROOT, "out", "rpcortex-v2-%s%s.bin" % (board, suffix))
    elf = os.path.join(ROOT, "os", "build_%s%s" % (board, suffix), "rpcortex_v2.elf")
    flash = os.path.getsize(binp)
    with open(os.path.join(os.path.dirname(elf), "fw_reserve.txt")) as fh:
        slot = int(fh.read().strip()) // 2
    out = subprocess.check_output(["arm-none-eabi-size", "-A", elf], text=True)
    ram = 0
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] in (".data", ".bss", ".tdata", ".tbss", ".uninitialized_data",
                                    ".ram_vector_table", ".scratch_x", ".scratch_y"):
            ram += int(p[1])
    return {"flash": flash, "ram": ram, "slot": slot}


def build_variant(boards, w, wo):
    """./build.sh for these boards and this variant. An image that builds but does not
    fit its slot stops build.sh at that board; it is noted and the rest are built in a
    second run. Anything else that fails is a real failure and stops the measuring.
    Returns the boards whose image did not fit."""
    over = []
    left = list(boards)
    while left:
        cmd = ["./build.sh"] + left
        if w:
            cmd += ["--with", ",".join(w)]
        if wo:
            cmd += ["--without", ",".join(wo)]
        print("measure: %s" % " ".join(cmd), flush=True)
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if p.returncode == 0:
            break
        at = [ln[4:].split()[0] for ln in p.stdout.splitlines() if ln.startswith("==> ")]
        if not at or "OVER the" not in p.stdout.split("==> " + at[-1], 1)[-1]:
            sys.stdout.write(p.stdout[-3000:])
            raise SystemExit("measure: %s failed" % " ".join(cmd))
        print("measure: %s does not fit its slot" % at[-1], flush=True)
        over.append(at[-1])
        left = left[left.index(at[-1]) + 1:]
    return over


def cmd_measure(a):
    """One build.sh run per variant, for the boards it applies to (the standard run is
    every board), then the .bin size and the static RAM of each image."""
    boards = a.boards or list(BOARDS)
    for b in boards:
        check_board(b)
    count = subprocess.check_output(["git", "rev-list", "--count", "HEAD"], cwd=ROOT, text=True).strip()
    sha = subprocess.check_output(["git", "rev-parse", "--short=8", "HEAD"], cwd=ROOT, text=True).strip()
    sizes = load_sizes() or {}
    sizes.setdefault("boards", {})
    recs = {b: {"variants": {}} for b in boards}
    for w, wo in measured_variants():
        tags = {b: variant(b, resolve(b, w, wo)[0]) for b in boards}
        standard_run = not w and not wo
        these = [b for b in boards if standard_run or tags[b]]
        if not these:
            continue
        over = build_variant(these, w, wo)
        for b in these:
            n = image_numbers(b, tags[b])
            if b in over:
                n["fits"] = False
            if standard_run:
                recs[b]["standard"] = n
            else:
                recs[b]["variants"][tags[b]] = n
    sizes["boards"].update(recs)
    sizes["measured_at"] = "build %s (%s)" % (count, sha)
    with open(SIZES, "w") as fh:
        json.dump(sizes, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print("measure: wrote %s" % os.path.relpath(SIZES, ROOT))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="What an RPCortex v2 image can be built with or without.")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="every feature, its boards and its size")
    r = sub.add_parser("resolve", help="shell assignments for build.sh")
    r.add_argument("board")
    r.add_argument("--with", dest="with_", action="append", default=[])
    r.add_argument("--without", action="append", default=[])
    c = sub.add_parser("cmake", help="settings for os/CMakeLists.txt")
    c.add_argument("board")
    c.add_argument("out")
    c.add_argument("settings", nargs="*")
    m = sub.add_parser("measure", help="build each variant and record its size")
    m.add_argument("boards", nargs="*")
    v = sub.add_parser("image-variant", help="the variant a built image says it is")
    v.add_argument("image")
    a = p.parse_args(argv)
    return {"list": cmd_list, "resolve": cmd_resolve, "cmake": cmd_cmake,
            "measure": cmd_measure, "image-variant": cmd_image_variant}[a.cmd](a)


def cmd_image_variant(a):
    with open(a.image, "rb") as fh:
        v = image_variant(fh.read())
    if v is None:
        sys.stderr.write("features: %s carries no feature marker\n" % a.image)
        return 1
    print(v)
    return 0


if __name__ == "__main__":
    sys.exit(main())
