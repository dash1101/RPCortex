"""Firmware: the running version against the latest release, and getting the
new one onto the board.

The release manifest is the one a device reads for `update check`
(releases/latest.json in the OS repository, written by tools/release.py): one
entry per board, `ver` as MAJOR.MINOR.BUILD ("2.0.640"), with the size and
SHA-256 of the raw .bin. The .uf2 for the boot ROM sits beside it, and its
SHA-256 is in releases.json, the release history the websites read.

The version scheme changed on 2026-11-21. v2.01-beta to v2.05-beta called
themselves vMAJOR.ITER ("v2.05-beta", build 635) and compare as 2.05.635, which
is NEWER than 2.0.640 number by number. So a device on one of them is told it
is up to date, and moves across once with `sudo update online --force`.
"""
import hashlib
import json
import os
import posixpath
import re
import shutil
import sys
import tempfile
import time

from rpctool import flash as _flash
from rpctool import repo as _repo
from rpctool import transport as _transport
from rpctool.errors import DigestMismatch, RpcError

from .parse import version_cmp

RELEASES_BASE = "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/"
MANIFEST_URL = RELEASES_BASE + "latest.json"        # MANIFEST_URL in os/shell/update_net.cpp
HISTORY_URL = RELEASES_BASE + "releases.json"

# The boot ROM's drive, by chip.
CHIPS = {"pico2_w": "RP2350", "pico2": "RP2350", "pico_w": "RP2040", "pico": "RP2040"}


class Running(object):
    """What the device says it runs, from HELLO (os=, build=)."""

    def __init__(self, hello):
        self.os = hello.get("os", "") or ""
        self.build = hello.get("build", "") or ""
        self.board = hello.get("board", "") or ""
        m = re.match(r"^v?(\d+)\.(\d+)(?:\.(\d+))?(.*)$", self.os)
        self.old_scheme = False
        if not m:
            self.comparable = ""
        elif m.group(3) is None:
            # vMAJOR.ITER with the build beside it: v2.01-beta to v2.05-beta,
            # and v2.00-beta. RPC_OS_BUILDVER was MAJOR.ITER.BUILD there.
            self.old_scheme = True
            self.comparable = "{}.{}.{}".format(m.group(1), m.group(2), self.build or "0")
        else:
            self.comparable = "{}.{}.{}".format(m.group(1), m.group(2), m.group(3))

    @property
    def display(self):
        if self.old_scheme and self.build:
            return "{} (build {})".format(self.os, self.build)
        return self.os or "unknown"


class Plan(object):
    """What the Firmware screen offers."""

    def __init__(self):
        self.latest = None          # manifest entry for this board (a dict)
        self.state = "unknown"      # current | update | newer | unknown | noboard
        self.force = False          # `update online` needs --force
        self.note = ""

    @property
    def update_available(self):
        return self.state == "update"


def plan(running, manifest):
    """Compare the running firmware with the manifest's entry for its board."""
    p = Plan()
    if manifest is None:
        p.note = "The release list has not been read yet."
        return p
    entry = board_entry(manifest, running.board)
    if entry is None:
        p.state = "noboard"
        p.note = "No release is listed for this board ({}).".format(running.board or "unknown")
        return p
    p.latest = entry
    ver = entry.get("ver", "")
    if not running.comparable:
        p.note = "The device did not say which version it runs."
        return p
    cmp_dev = version_cmp(ver, running.comparable)
    if running.old_scheme and _major(ver) == _major(running.comparable) and _new_scheme(ver):
        # Every three-number release came after every vMAJOR.ITER one, so this
        # is an update whatever the numbers say; the device itself compares
        # them number by number and calls it older.
        p.state = "update"
        p.force = cmp_dev <= 0
        if p.force:
            p.note = ("This device runs {}, from before the version numbers changed on "
                      "2026-11-21. It reads {} as older than itself, so this one update "
                      "needs --force; after it, updates are offered as usual."
                      .format(running.display, display_ver(ver)))
        return p
    if cmp_dev > 0:
        p.state = "update"
    elif cmp_dev == 0:
        p.state = "current"
    else:
        p.state = "newer"
        p.note = "The device runs a newer build than the latest release (a development image)."
    return p


def _major(v):
    return (v or "").split(".", 1)[0]


def _new_scheme(v):
    return bool(re.match(r"^\d+\.\d+\.\d+$", v or ""))


def display_ver(ver):
    return "v" + ver if ver and not ver.startswith("v") else (ver or "?")


def board_entry(manifest, board):
    for e in (manifest or {}).get("packages", []):
        if isinstance(e, dict) and e.get("name") == board:
            return e
    return None


def update_line(dev, force):
    """The command that updates over the device's own network. --yes because
    nothing can answer the device's own question in link mode; the TUI asks
    first, naming the version."""
    return dev.sudo_line("update online --yes" + (" --force" if force else ""))


def read_source(source, progress=None):
    """The bytes at a URL (downloaded, certificates checked) or a local path."""
    if re.match(r"^(https?|file)://", source or ""):
        return _repo.fetch(source, progress)
    try:
        with open(source, "rb") as f:
            data = f.read()
    except OSError as e:
        raise RpcError("cannot read {}: {}".format(source, e))
    if progress:
        progress(len(data), len(data))
    return data


def flashed_message(name, drive):
    how = "Loaded {} with picotool".format(name) if drive == "picotool" else \
        "Wrote {} to {}".format(name, drive)
    return how + ". The board restarts with its files kept; reconnecting when it is back."


def load_json(source, progress=None):
    data = read_source(source, progress)
    try:
        return json.loads(data.decode("utf-8"))
    except ValueError as e:
        raise RpcError("{} is not JSON: {}".format(source, e))


def uf2_source(entry, history=None):
    """(url, sha256 or None) of the .uf2 that goes with a manifest entry. The
    URL is the .bin's with the other extension (tools/release.py writes both
    side by side); the digest comes from the newest release in the history
    whose build matches."""
    url = entry.get("url", "")
    if url.endswith(".bin"):
        url = url[:-4] + ".uf2"
    else:
        url = posixpath.join(posixpath.dirname(url), "rpcortex-v2-{}.uf2".format(entry.get("name")))
    sha = None
    build = (entry.get("ver", "").split(".") + [""])[2] if entry.get("ver") else ""
    for rel in (history or {}).get("releases", []):
        if build and str(rel.get("build", "")) != build:
            continue
        for a in rel.get("assets", []):
            if a.get("board") == entry.get("name") and a.get("kind") == "uf2" and a.get("sha256"):
                sha = a["sha256"].lower()
                break
        if sha:
            break
    return url, sha


def download_uf2(url, sha, progress=None):
    """The image, checked against `sha` when there is one. Returns (data, verified)."""
    data = read_source(url, progress)
    if sha:
        have = hashlib.sha256(data).hexdigest()
        if have != sha:
            raise DigestMismatch("The .uf2 does not match the release's checksum.\n"
                                 "  expected  {}\n  received  {}\nNothing was written."
                                 .format(sha, have))
        _flash.uf2_chip(data)
        return data, True
    _flash.uf2_chip(data)
    return data, False


class Flasher(object):
    """The BOOTSEL path of `rpc flash`, in steps a worker can report and a test
    can replace: reset into BOOTSEL by opening the port at 1200 baud, wait for
    the boot ROM's drive, check the chip, copy the image on."""

    def __init__(self, touch_1200=None, find_drives=None, copy_uf2=None, drive_chip=None,
                 picotool_load=None, bootsel_on_usb=None, sleep=time.sleep):
        # Looked up at call time, not bound here, so a test's patch of the
        # library function is what runs.
        self.touch_1200 = touch_1200 or (lambda p: _transport.touch_1200(p))
        self.find_drives = find_drives or (lambda: _flash.find_drives())
        self.copy_uf2 = copy_uf2 or (lambda d, drive, name: _flash.copy_uf2(d, drive, name))
        self.drive_chip = drive_chip or (lambda d: _flash.drive_chip(d))
        # As `rpc flash`: on Linux, picotool when nothing mounted the drive.
        self.picotool_load = picotool_load or (
            lambda path: sys.platform.startswith("linux") and _flash.picotool_load(path))
        self.bootsel_on_usb = bootsel_on_usb or (lambda: _flash.bootsel_on_usb())
        self.sleep = sleep

    def flash(self, data, port_path, name="firmware.uf2", wait=20.0, step=None, cancelled=None):
        """Returns the drive written to. `step(text)` reports progress;
        `cancelled()` is asked while waiting."""
        step = step or (lambda s: None)
        chip = _flash.uf2_chip(data)
        present = list(self.find_drives())
        # With no port, the board is already in BOOTSEL (held while plugging in).
        drive = None if port_path else (present[0] if present else None)
        if port_path:
            step("Resetting the board into BOOTSEL...")
            self.touch_1200(port_path)
        if drive is None:
            step("Waiting for the BOOTSEL drive...")
            end = time.monotonic() + wait
            while drive is None:
                for d in self.find_drives():
                    drive = d
                    break
                if drive or time.monotonic() >= end:
                    break
                if cancelled and cancelled():
                    raise RpcError("cancelled while waiting for the BOOTSEL drive")
                self.sleep(0.5)
        if drive is None:
            step("No drive mounted; trying picotool...")
            if self._picotool(data, name):
                return "picotool"
            if self.bootsel_on_usb():
                raise RpcError("The board is in BOOTSEL, but nothing mounted its drive. Mount "
                               "the drive labelled RP2350 or RPI-RP2 (or install picotool), "
                               "then flash again; the board waits in BOOTSEL until then.")
            raise RpcError("No BOOTSEL drive appeared. Hold BOOTSEL while plugging the board "
                           "in, or mount the drive labelled RP2350 or RPI-RP2, then try again.")
        dchip = self.drive_chip(drive)
        if chip and dchip and chip != dchip:
            raise RpcError("This image is for {}, but the board on {} is an {}. Nothing was "
                           "written.".format(chip, drive, dchip))
        step("Writing {} to {}...".format(name, drive))
        self.copy_uf2(data, drive, name)
        return drive

    def _picotool(self, data, name):
        """picotool load + reboot from a temporary copy of the image. False
        when there is no picotool; raises when it ran and failed."""
        d = tempfile.mkdtemp(prefix="rpc-uf2-")
        try:
            path = os.path.join(d, os.path.basename(name) or "firmware.uf2")
            with open(path, "wb") as f:
                f.write(data)
            return bool(self.picotool_load(path))
        finally:
            shutil.rmtree(d, True)
