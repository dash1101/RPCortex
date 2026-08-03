#!/usr/bin/env python3
"""Verify the firmware-writing routine can survive erasing its own flash.

apply_staged runs while the flash holding the rest of the firmware is being
erased and rewritten. Everything it executes has to already be in RAM, and so
does everything it calls. Get that wrong and the device locks up mid-write with
no output, needing BOOTSEL — which is exactly what happened twice:

  * __not_in_flash_func places a function in RAM but does NOT stop the compiler
    inlining it into a caller that lives in flash. A static function called once
    is inlined every time.
  * watchdog_reboot() lives in flash, so rebooting through it branched into a
    region erased seconds earlier.

Neither is visible in source review; both are obvious in the symbol table. So
this checks the built image rather than trusting the annotations.
"""
import re
import subprocess
import sys

RAM_LO, RAM_HI = 0x20000000, 0x20090000


def main():
    elf, nm, objdump = sys.argv[1], sys.argv[2], sys.argv[3]

    syms = subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout
    target = None
    for line in syms.splitlines():
        p = line.split()
        if len(p) >= 3 and "apply_staged" in p[2] and "veneer" not in p[2]:
            target = (int(p[0], 16), p[2])
    if not target:
        print("  FAIL apply_staged is not in the image at all — inlined away?")
        return 1

    addr, name = target
    if not (RAM_LO <= addr < RAM_HI):
        print("  FAIL apply_staged is at %08x, which is FLASH." % addr)
        print("       It erases that flash. Use __no_inline_not_in_flash_func.")
        return 1

    # Every call it makes must also land in RAM.
    dis = subprocess.run(
        [objdump, "-d", "--start-address=%d" % addr, "--stop-address=%d" % (addr + 0x200), elf],
        capture_output=True, text=True).stdout

    bad = []
    for line in dis.splitlines():
        m = re.search(r"\s(bl|blx|b\.w)\s+([0-9a-f]+)\s+<([^>]+)>", line)
        if not m:
            continue
        dest, sym = int(m.group(2), 16), m.group(3)
        if not (RAM_LO <= dest < RAM_HI):
            bad.append("%s at %08x" % (sym, dest))

    if bad:
        print("  FAIL apply_staged calls into flash: %s" % ", ".join(bad))
        print("       Those addresses are erased by the time they are reached.")
        return 1

    print("  flashsafe        ok   apply_staged at %08x, all calls in RAM" % addr)
    return 0


sys.exit(main())
