#!/usr/bin/env python3
"""Where the firmware image goes, grouped by component.

build.sh already prints the one number that gates a release — how much of the
slot is left. This is the ranked detail underneath it: the flash cost of each
component (our code, the radio driver and its firmware blob, the BT stack, TLS,
the network stack, littlefs, newlib), so a pass that grew the image can be
pointed at what grew rather than guessed about.

Reads the linker map, not the ELF: the map is what attributes bytes to the
object they came from. One wrinkle it handles — GCC merges identical string
literals into a shared pool (.rodata.str*), and ld reports the whole pool
against the first object that contributed to it, with the real per-object share
on a following "(size before relaxing)" line. Left uncorrected that pins ~90 KB
of everyone's strings onto whichever file linked first. The correction below
uses the relaxed size, so the attribution is honest.

Flash only: .text/.rodata/.data are counted, .bss is not (it is RAM). The total
lands within a percent or two of the .bin because merged-string dedup saves a
little the per-object sum cannot see.

    tools/size-report.py [path/to/rpcortex_v2.elf.map] [--groups]

Default map is os/build_pico2_w/rpcortex_v2.elf.map — the tight board.
--groups prints only the component table (no per-file list); build.sh uses it
to print a component breakdown under each board's headroom line.
"""
import re
import sys
from collections import defaultdict

DEFAULT_MAP = "os/build_pico2_w/rpcortex_v2.elf.map"


def classify(name):
    if name.startswith(".text"):
        return "flash"
    if name.startswith(".rodata") or name.startswith(".data") or name.startswith(".ARM"):
        return "flash"
    return "skip"


def bucket(obj):
    o = obj.lower()
    if "cyw43" in o:
        return "cyw43 (radio driver + fw blob)"
    if "/lib/btstack" in o or "pico_btstack" in o or "btstack" in o:
        return "btstack (Bluetooth)"
    if "/lib/mbedtls" in o or "mbedcrypto" in o:
        return "mbedtls (TLS)"
    if "/lib/lwip" in o or "lwip" in o:
        return "lwip (network)"
    if "littlefs" in o or "/lfs" in o:
        return "littlefs"
    if "loader-spike" in o:
        return "loader-spike (shared loader)"
    if "rpcortex_v2.dir" in obj:
        if "/sdk/" in obj:
            return "pico-sdk"
        return "OS code (our source)"
    if "libgcc" in o:
        return "libgcc"
    if "libstdc++" in o:
        return "libstdc++"
    if "libm.a" in o:
        return "libm"
    if "libc.a" in o or "libg.a" in o:
        return "newlib (libc)"
    if ".a(" in obj:
        return "pico-sdk"
    return "other"


def parse(mapfile):
    lines = open(mapfile, errors="replace").read().splitlines()
    start = 0
    for i, l in enumerate(lines):
        if l.startswith("Linker script and memory map"):
            start = i + 1
            break

    re_full = re.compile(r"^\s+(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    re_name = re.compile(r"^\s+(\.\S+)\s*$")
    re_addr = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    re_relax = re.compile(r"^\s+0x([0-9a-f]+)\s+\(size before relaxing\)")

    records = []          # (section, size, obj), in order
    pending = None
    for l in lines[start:]:
        if l.startswith("OUTPUT(") or l.startswith("LOAD "):
            continue
        m = re_relax.match(l)
        if m and records:
            nm_, _, ob_ = records[-1]
            records[-1] = (nm_, int(m.group(1), 16), ob_)
            continue
        m = re_full.match(l)
        if m:
            name, size, obj = m.group(1), int(m.group(3), 16), m.group(4).strip()
            pending = None
        else:
            m = re_name.match(l)
            if m:
                pending = m.group(1)
                continue
            m = re_addr.match(l)
            if m and pending:
                name, size, obj = pending, int(m.group(2), 16), m.group(3).strip()
                pending = None
            else:
                continue
        if obj.startswith("0x"):
            continue
        records.append((name, size, obj))

    groups = defaultdict(int)
    os_objs = defaultdict(int)
    total = 0
    for name, size, obj in records:
        if size == 0 or classify(name) != "flash":
            continue
        b = bucket(obj)
        groups[b] += size
        total += size
        if b == "OS code (our source)":
            short = obj.split("rpcortex_v2.dir/")[-1].replace(".obj", "").replace("__/", "../")
            os_objs[short] += size
    return groups, os_objs, total


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    brief = "--groups" in sys.argv[1:]
    mapfile = args[0] if args else DEFAULT_MAP
    try:
        groups, os_objs, total = parse(mapfile)
    except FileNotFoundError:
        sys.stderr.write("size-report: no map at {} (build first)\n".format(mapfile))
        return 1

    print("  flash by component  ({})".format(mapfile))
    for b, s in sorted(groups.items(), key=lambda x: -x[1]):
        print("    {:>8.1f} KB  {}".format(s / 1024, b))
    print("    {:>8.1f} KB  TOTAL".format(total / 1024))
    if brief:
        return 0
    print()
    print("  OS source, top 15 translation units")
    for o, s in sorted(os_objs.items(), key=lambda x: -x[1])[:15]:
        print("    {:>8.1f} KB  {}".format(s / 1024, o))
    return 0


if __name__ == "__main__":
    sys.exit(main())
