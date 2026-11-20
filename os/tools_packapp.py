#!/usr/bin/env python3
"""Make a built .app smaller without changing what the loader sees.

    tools_packapp.py <in.app> <out.app> <objcopy>

A package is a relocatable ELF object. Most of a large one is names: Nova D1's
symbol names were 68 KB of a 440 KB file, nearly all of them long mangled C++
names for functions nothing on the device ever looks up. The loader reads a
symbol's NAME in exactly two cases (loader-spike/firmware/loader.cpp):

  * an undefined symbol: a firmware call the package imports, found by name;
  * app_main, the entry point, found by name.

Every other symbol is used by its section and value only. So:

  1. objcopy --strip-unneeded --keep-symbol=app_main -R .comment
     drops symbols no relocation refers to, and the compiler's version string.
     objcopy renumbers sections and symbols itself, which is why it does this
     part rather than this script.
  2. Every remaining DEFINED symbol other than app_main loses its name, and the
     string table is rebuilt with only the names still used.
  3. The file is laid out again so the space those names took is gone.

The result is still an ordinary relocatable ELF object (readelf and objdump read
it), with the same sections in the same order, the same symbols at the same
indices and the same relocations. Only names nobody reads are gone.

Exit status is non-zero, and nothing is written, on anything unexpected.
"""
import os
import struct
import subprocess
import sys
import tempfile

SHT_SYMTAB = 2
SHT_NOBITS = 8


def die(msg):
    sys.exit("tools_packapp: " + msg)


def pack(data):
    b = bytes(data)
    if b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 1:
        die("not a 32-bit little-endian ELF file")
    e_type, = struct.unpack_from("<H", b, 0x10)
    if e_type != 1:
        die("not a relocatable object (ET_REL)")
    e_shoff, = struct.unpack_from("<I", b, 0x20)
    e_phnum, = struct.unpack_from("<H", b, 0x2C)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    if e_phnum != 0 or e_shentsize != 40:
        die("unexpected program headers or section header size")

    # name, type, flags, addr, offset, size, link, info, addralign, entsize
    sh = [list(struct.unpack_from("<10I", b, e_shoff + i * 40)) for i in range(e_shnum)]
    symtabs = [i for i, s in enumerate(sh) if s[1] == SHT_SYMTAB]
    if len(symtabs) != 1:
        die("expected exactly one symbol table")
    symi = symtabs[0]
    stri = sh[symi][6]
    if stri == e_shstrndx:
        die("the symbol names share the section-name table")

    old = b[sh[stri][4]: sh[stri][4] + sh[stri][5]]

    def name_at(off):
        end = old.find(b"\0", off)
        return old[off:end] if end >= 0 else b""

    # Rebuild the symbol table's names.
    symdata = bytearray(b[sh[symi][4]: sh[symi][4] + sh[symi][5]])
    if len(symdata) % 16:
        die("symbol table size is not a multiple of 16")
    strtab = bytearray(b"\0")
    kept = {}
    for k in range(len(symdata) // 16):
        st_name, = struct.unpack_from("<I", symdata, k * 16)
        st_shndx, = struct.unpack_from("<H", symdata, k * 16 + 14)
        name = name_at(st_name) if st_name else b""
        if name and (st_shndx == 0 or name == b"app_main"):
            if name not in kept:
                kept[name] = len(strtab)
                strtab += name + b"\0"
            new = kept[name]
        else:
            new = 0
        struct.pack_into("<I", symdata, k * 16, new)
    if b"app_main" not in kept:
        die("app_main is missing; the loader could not find the entry point")

    # Lay the file out again: header, each section's contents in their original
    # order and alignment, then the section headers.
    content = {}
    for i, s in enumerate(sh):
        if i == 0 or s[1] == SHT_NOBITS:
            continue
        if i == symi:
            content[i] = bytes(symdata)
        elif i == stri:
            content[i] = bytes(strtab)
        else:
            content[i] = b[s[4]: s[4] + s[5]]

    out = bytearray(b[:52])
    for i in sorted(content, key=lambda j: sh[j][4]):
        align = max(1, sh[i][8])
        while len(out) % align:
            out += b"\0"
        sh[i][4] = len(out)
        sh[i][5] = len(content[i])
        out += content[i]
    for i, s in enumerate(sh):
        if s[1] == SHT_NOBITS:
            s[4] = 0 if i == 0 else len(out)    # NOBITS takes no file space
    while len(out) % 4:
        out += b"\0"
    shoff = len(out)
    for s in sh:
        out += struct.pack("<10I", *s)
    struct.pack_into("<I", out, 0x20, shoff)
    return bytes(out), len(kept)


def main():
    if len(sys.argv) != 4:
        die("usage: tools_packapp.py <in.app> <out.app> <objcopy>")
    src, dst, objcopy = sys.argv[1:]
    before = os.path.getsize(src)
    fd, tmp = tempfile.mkstemp(suffix=".app")
    os.close(fd)
    try:
        r = subprocess.run([objcopy, "--strip-unneeded", "--keep-symbol=app_main",
                            "-R", ".comment", src, tmp],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            die("objcopy failed: " + r.stdout.decode(errors="replace"))
        with open(tmp, "rb") as f:
            packed, names = pack(f.read())
    finally:
        os.unlink(tmp)
    with open(dst + ".tmp", "wb") as f:
        f.write(packed)
    os.replace(dst + ".tmp", dst)
    print("packapp: %s %d -> %d bytes (%d%% smaller, %d names kept)" % (
        os.path.basename(dst), before, len(packed),
        100 - (100 * len(packed) // before) if before else 0, names))


if __name__ == "__main__":
    main()
