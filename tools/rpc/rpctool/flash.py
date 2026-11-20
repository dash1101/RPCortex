"""Putting a firmware image on a board through its BOOTSEL drive.

Opening the console at 1200 baud makes the board reboot into the boot ROM,
which shows up as a USB drive (RPI-RP2 for an RP2040, RP2350 for an RP2350).
Copying a .uf2 onto that drive writes the image and restarts the board. The
image covers the firmware only; the filesystem, and every file on it, is left
alone.
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import time

from .errors import RpcError

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000

# sdk/src/common/boot_uf2_headers/include/boot/uf2.h
FAMILIES = {
    0xE48BFF56: "RP2040",
    0xE48BFF59: "RP2350",   # RP2350_ARM_S
    0xE48BFF5A: "RP2350",   # RP2350_RISCV
    0xE48BFF5B: "RP2350",   # RP2350_ARM_NS
}
# ABSOLUTE (0xE48BFF57) and DATA (0xE48BFF58) say nothing about the chip; an
# RP2350 image opens with one absolute block that the ROM is told to ignore.


def uf2_chip(data):
    """The chip a UF2 image is for: 'RP2040', 'RP2350' or None if it does not say.
    Raises RpcError when it is not a UF2 file at all."""
    if len(data) < 512 or len(data) % 512:
        raise RpcError("not a UF2 image (the size is not a whole number of 512-byte blocks)")
    chips = set()
    for off in range(0, len(data), 512):
        m0, m1, flags = struct.unpack_from("<III", data, off)
        end, = struct.unpack_from("<I", data, off + 508)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or end != UF2_MAGIC_END:
            raise RpcError("not a UF2 image (block {} has no UF2 magic)".format(off // 512))
        if flags & UF2_FLAG_FAMILY:
            fam, = struct.unpack_from("<I", data, off + 28)
            if fam in FAMILIES:
                chips.add(FAMILIES[fam])
    if len(chips) > 1:
        raise RpcError("the UF2 image mixes RP2040 and RP2350 blocks")
    return chips.pop() if chips else None


def drive_chip(drive):
    """'RP2040' or 'RP2350' from the drive's INFO_UF2.TXT, or None."""
    try:
        with open(os.path.join(drive, "INFO_UF2.TXT"), "r") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError):
        return None
    if "RP2350" in text:
        return "RP2350"
    if "RPI-RP2" in text or "RP2040" in text:
        return "RP2040"
    return None


def _mounts_linux():
    points = []
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) > 1:
                    points.append(parts[1].replace("\\040", " "))
    except OSError:
        pass
    user = os.environ.get("USER", "")
    for pat in ("/media/*", "/media/{}/*".format(user), "/run/media/{}/*".format(user), "/mnt/*"):
        points.extend(glob.glob(pat))
    return points


def _drives_windows():
    import ctypes
    import string
    try:
        ctypes.windll.kernel32.SetErrorMode(1)       # no "insert a disk" dialogs
        mask = ctypes.windll.kernel32.GetLogicalDrives()
    except Exception:
        mask = (1 << 26) - 1
    return ["{}:\\".format(c) for i, c in enumerate(string.ascii_uppercase)
            if mask & (1 << i) and c not in "AB"]


def find_drives():
    """Mounted BOOTSEL drives: every mount point holding an INFO_UF2.TXT."""
    if sys.platform.startswith("win"):
        cands = _drives_windows()
    elif sys.platform == "darwin":
        cands = glob.glob("/Volumes/*")
    else:
        cands = _mounts_linux()
    seen, out = set(), []
    for c in cands:
        if c in seen:
            continue
        seen.add(c)
        try:
            if os.path.isfile(os.path.join(c, "INFO_UF2.TXT")):
                out.append(c)
        except OSError:
            pass
    return out


def bootsel_on_usb():
    """On Linux, whether a board in BOOTSEL is on the bus even if nothing has
    mounted its drive. None where that cannot be told."""
    if not sys.platform.startswith("linux"):
        return None
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(os.path.join(d, "idVendor")) as f:
                vid = f.read().strip().lower()
            with open(os.path.join(d, "idProduct")) as f:
                pid = f.read().strip().lower()
        except OSError:
            continue
        if vid == "2e8a" and pid in ("0003", "000f"):
            return True
    return False


def wait_for_drive(timeout, ignore=(), poll=0.5, finder=None):
    finder = finder or find_drives
    end = time.monotonic() + timeout
    while True:
        for d in finder():
            if d not in ignore:
                return d
        if time.monotonic() >= end:
            return None
        time.sleep(poll)


def copy_uf2(data, drive, name="firmware.uf2"):
    """Write the image onto the drive. The board restarts as the last block
    lands, and the drive can vanish before the write is closed; that is the
    expected ending, not a failure."""
    dest = os.path.join(drive, name)
    written = 0
    try:
        with open(dest, "wb") as f:
            for off in range(0, len(data), 64 * 1024):
                f.write(data[off:off + 64 * 1024])
                written = off + len(data[off:off + 64 * 1024])
            f.flush()
            try:
                os.fsync(f.fileno())
            except OSError:
                pass
    except OSError as e:
        if written < len(data):
            raise RpcError("copying to {} failed after {} of {} bytes: {}"
                           .format(drive, written, len(data), e))
    return dest


def _quiet(cmd):
    """Run a command, keeping its output unless it fails. picotool redraws a
    progress bar with carriage returns, which a log or a pipe shows as one long
    line; on success none of it is worth seeing."""
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        text = r.stdout.decode("utf-8", "replace")
        lines = [ln.split("\r")[-1] for ln in text.splitlines()]
        sys.stderr.write("\n".join(lines[-20:]) + "\n")
    return r.returncode


def picotool_load(uf2_path, runner=None, which=None):
    """`picotool load <uf2>` then `picotool reboot`. False when picotool is not
    installed; raises when it is and fails."""
    which = which or shutil.which
    runner = runner or _quiet
    tool = which("picotool")
    if not tool:
        return False
    if runner([tool, "load", uf2_path]) != 0:
        raise RpcError("picotool load failed (see above)")
    runner([tool, "reboot"])
    return True
