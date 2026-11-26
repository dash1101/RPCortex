#!/usr/bin/env python3
"""The .uf2 for an RP2350 board with A/B firmware partitions.

    tools_ab_uf2.py <partition table .bin> <image .bin> <A offset> <B offset> <out .uf2>

One UF2 in the ABSOLUTE family (0xe48bff57), so the boot ROM writes it where it
says regardless of what is on the board now (RP2350 datasheet 5.1.18):

  - the partition table at flash offset 0 (slot 0),
  - slot 1 (the next 4 KB) blank, so no stale table is ever found there,
  - the image at the start of partition A,
  - the first 4 KB of partition B blank, so whatever B held (an older image, a
    staged update, the USB drive's files) cannot be booted in place of A.

Flashed by dragging it onto the BOOTSEL drive or with `picotool load`, it leaves
any board, fresh or running older firmware, booting this image from A.
"""
import struct
import sys

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
ABSOLUTE = 0xE48BFF57
XIP = 0x10000000
PAYLOAD = 256
SECTOR = 4096


def main():
    pt_path, img_path, a_off, b_off, out = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5]
    pt = open(pt_path, "rb").read()
    img = open(img_path, "rb").read()
    if len(pt) > SECTOR:
        sys.exit("partition table is %d bytes; slot 0 holds %d" % (len(pt), SECTOR))
    if a_off < 2 * SECTOR or b_off <= a_off or len(img) > b_off - a_off:
        sys.exit("image of %d bytes does not fit partition A (%#x..%#x)" % (len(img), a_off, b_off))

    regions = [
        (0, pt + b"\xff" * (SECTOR - len(pt))),          # slot 0: the table
        (SECTOR, b"\xff" * SECTOR),                        # slot 1: nothing
        (a_off, img),                                      # partition A: the image
        (b_off, b"\xff" * SECTOR),                         # partition B: not bootable
    ]
    blocks = []
    for off, data in regions:
        if len(data) % PAYLOAD:
            data += b"\xff" * (PAYLOAD - len(data) % PAYLOAD)
        for i in range(0, len(data), PAYLOAD):
            blocks.append((XIP + off + i, data[i:i + PAYLOAD]))

    with open(out, "wb") as f:
        for n, (addr, chunk) in enumerate(blocks):
            hdr = struct.pack("<IIIIIIII", UF2_MAGIC0, UF2_MAGIC1, FLAG_FAMILY, addr,
                              PAYLOAD, n, len(blocks), ABSOLUTE)
            f.write(hdr + chunk + b"\x00" * (476 - PAYLOAD) + struct.pack("<I", UF2_MAGIC_END))
    print("%s: table, image %d KB at %#x, B blanked at %#x (%d blocks)"
          % (out, len(img) // 1024, a_off, b_off, len(blocks)))


if __name__ == "__main__":
    main()
