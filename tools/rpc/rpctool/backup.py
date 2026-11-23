"""Backing up a device's files to the computer, and putting them back.

What is kept: everything a person put on the device. That is /home, /etc,
/usb and anything else at the top level, but not /os (the system's own
files: settings, accounts, the device key, firmware images), /tmp (emptied at
every boot) or /sd (a card, which is read-only here). Installed packages,
which live in /os/pkg, are added with --with-packages; restoring them installs
them again with `pkg install`.

--with-system adds the system's own settings: the registry (settings, saved
Wi-Fi networks, aliases), the account file, the device key and the SSH host
key. Reading the accounts and the keys needs sudo, which AUTH gives. They are
kept apart from the files, under system/, so a restore writes them only when
asked to, after saying what they replace.

A backup is a directory or a .zip with the same layout:

    rpc-backup.json         what was backed up, from which device, with digests
    files/<path>            each file, at its path on the device
    packages/<name>.app     installed packages (only with --with-packages)
    system/os/<name>        system files (only with --with-system)
"""
import datetime
import hashlib
import json
import os
import posixpath
import time
import zipfile

from .errors import DeviceGone, LinkError, RpcError
from .link import _b64
from .pkg import install_bytes

MANIFEST = "rpc-backup.json"
SKIP_TOP = ("os", "tmp", "sd")

# What --with-system adds. The registry and the account file each have a
# shadow copy (.bak) the OS writes beside them; a restore writes the shadow
# too, so the pair agrees, as it does after every save on the device.
SYSTEM_FILES = ("/os/registry.cfg", "/os/users.cfg", "/os/device.key")
SYSTEM_TREES = ("/os/ssh",)
SHADOWS = {"/os/registry.cfg": "/os/registry.bak", "/os/users.cfg": "/os/users.bak"}

SYSTEM_WHAT = {
    "/os/registry.cfg": "the settings: saved Wi-Fi networks, aliases, startup choices",
    "/os/users.cfg": "the accounts and their passwords",
    "/os/device.key": "the device key, which seals the saved Wi-Fi passwords",
}


def not_included(with_packages=False, with_system=False):
    """What a backup deliberately leaves out, each with the option that adds
    it (None where nothing does)."""
    out = []
    if not with_system:
        out.append(("the settings, saved Wi-Fi networks, accounts, device key and SSH host key",
                    "--with-system"))
    if not with_packages:
        out.append(("the installed packages", "--with-packages"))
    out.append(("/tmp, emptied at every start, and /sd, a card", None))
    out.append(("the firmware itself ('rpc flash' puts it back)", None))
    return out


def system_restore_notice(paths):
    """The confirmation a restore of system files asks for, naming each file."""
    lines = ["This restore replaces system files on the device:"]
    for p in paths:
        what = SYSTEM_WHAT.get(p)
        if what is None and p.startswith("/os/ssh/"):
            what = "the SSH host key"
        lines.append("  {}  ({})".format(p, what) if what else "  " + p)
    lines += [
        "The accounts and passwords become the ones in the backup, so the password",
        "sudo asks for may change.",
        "Saved Wi-Fi passwords are sealed under the device key, and only the key they",
        "were sealed with opens them. Restored onto a different device, they work only",
        "with the device key from this backup; without it, the networks have no password.",
        "The device then restarts at once, without saving: a normal restart would",
        "write the settings it holds in memory over the restored ones. Its next start",
        "may say it did not shut down cleanly.",
    ]
    return "\n".join(lines)


SENSITIVE_NOTE = ("This backup holds the account file and the device key, which opens the "
                  "saved Wi-Fi passwords in it. Keep it as safe as the device itself.")


def restart_unsaved(dev):
    """Restart without saving anything (`sreboot`), so the registry the running
    OS holds in memory is not written over a restored one. `reboot` saves it on
    the way out (clock_persist), which would undo the restore; the backup
    package resets without saving for the same reason. Returns once the device
    has gone. DEVICE-UNCONFIRMED: that the next start reads the restored files
    and what it reports about the unclean restart."""
    if dev.mode != "link":
        raise RpcError("restarting after a restore needs link mode")
    dev.link.send("EXEC sreboot")
    out = bytearray()
    end = time.monotonic() + 4.0
    try:
        while time.monotonic() < end:
            f = dev.link.r.next_frame(0.3)
            if f is None:
                continue
            if f.type == "OUT":
                out.extend(_b64(f.rest))
            elif f.final:
                # Still here: refused (an ordinary account). Say what it said.
                raise RpcError(bytes(out).decode("utf-8", "replace").strip()
                               or "the device did not restart")
    except DeviceGone:
        pass
    dev.gone = True


class _DirStore(object):
    def __init__(self, root, write):
        self.root = root
        if write:
            os.makedirs(root, exist_ok=True)

    def _local(self, name):
        return os.path.join(self.root, *name.split("/"))

    def write(self, name, data):
        p = self._local(name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def mkdir(self, name):
        os.makedirs(self._local(name), exist_ok=True)

    def read(self, name):
        with open(self._local(name), "rb") as f:
            return f.read()

    def names(self):
        out = []
        for dirpath, dirnames, filenames in os.walk(self.root):
            rel = os.path.relpath(dirpath, self.root)
            rel = "" if rel == "." else rel.replace(os.sep, "/") + "/"
            for d in dirnames:
                out.append(rel + d + "/")
            for f in filenames:
                out.append(rel + f)
        return sorted(out)

    def close(self):
        pass


class _ZipStore(object):
    def __init__(self, path, write):
        self.z = zipfile.ZipFile(path, "w" if write else "r", zipfile.ZIP_DEFLATED)

    def write(self, name, data):
        self.z.writestr(name, data)

    def mkdir(self, name):
        self.z.writestr(name.rstrip("/") + "/", b"")

    def read(self, name):
        return self.z.read(name)

    def names(self):
        return sorted(self.z.namelist())

    def close(self):
        self.z.close()


def _store(path, write):
    if path.lower().endswith(".zip"):
        if not write and not os.path.isfile(path):
            raise RpcError("no backup at {}".format(path))
        return _ZipStore(path, write)
    if not write and not os.path.isdir(path):
        raise RpcError("no backup at {}".format(path))
    return _DirStore(path, write)


def backup(dev, dest, with_packages=False, log=None, warn=None, progress=None,
           with_system=False):
    """Copy the device's files into `dest` (a directory, or a path ending .zip).
    Returns (files, bytes, skipped)."""
    log = log or (lambda s: None)
    warn = warn or log
    manifest = {"format": 1,
                "created": datetime.datetime.now().replace(microsecond=0).isoformat(),
                "device": dev.hello, "files": [], "dirs": [], "packages": []}
    if with_system:
        manifest["system"] = []
    store = _store(dest, True)
    total = skipped = 0
    try:
        tops = dev.ls("/")
        roots = []
        for e in tops:
            if e.is_dir and e.name in SKIP_TOP:
                continue
            roots.append(e)

        def keep(path, data):
            store.write("files" + path, data)
            manifest["files"].append({"path": path, "size": len(data),
                                      "sha256": hashlib.sha256(data).hexdigest()})

        def fetch(path, size):
            try:
                return dev.get(path, progress(path, size) if progress else None)
            except LinkError as e:
                if e.code in ("perm", "nofile", "io", "toolong"):
                    warn("skipped {}: {}".format(path, e))
                    return None
                raise

        for e in roots:
            path = "/" + e.name
            if not e.is_dir:
                data = fetch(path, e.size)
                if data is None:
                    skipped += 1
                    continue
                keep(path, data)
                total += len(data)
                log("  {}".format(path))
                continue
            try:
                for d, ents in dev.walk(path):
                    manifest["dirs"].append(d)
                    store.mkdir("files" + d)
                    for f in ents:
                        if f.is_dir:
                            continue
                        fp = posixpath.join(d, f.name)
                        data = fetch(fp, f.size)
                        if data is None:
                            skipped += 1
                            continue
                        keep(fp, data)
                        total += len(data)
                        log("  {}".format(fp))
            except LinkError as err:
                if err.code not in ("perm", "nofile", "io"):
                    raise
                warn("skipped {}: {}".format(path, err))
                skipped += 1

        if with_packages:
            for e in dev.ls("/os/pkg"):
                if e.is_dir or not e.name.endswith(".app"):
                    continue
                data = fetch("/os/pkg/" + e.name, e.size)
                if data is None:
                    skipped += 1
                    continue
                store.write("packages/" + e.name, data)
                manifest["packages"].append({"name": e.name, "size": len(data),
                                             "sha256": hashlib.sha256(data).hexdigest()})
                total += len(data)
                log("  /os/pkg/{}".format(e.name))

        if with_system:
            n, size, bad = _backup_system(dev, store, manifest["system"], fetch, log, warn)
            total += size
            skipped += bad

        store.write(MANIFEST, json.dumps(manifest, indent=2).encode("utf-8"))
    finally:
        store.close()
    return (len(manifest["files"]) + len(manifest["packages"]) + len(manifest.get("system", [])),
            total, skipped)


def _backup_system(dev, store, entries, fetch, log, warn):
    """The system files, through sudo where the device asks for it (the
    account file, the device key and /os/ssh; Device sends AUTH and repeats
    the request). A file the image does not have (no SSH, a key never made) is
    not a failure. Returns (files, bytes, skipped)."""
    paths = []
    skipped = 0
    for p in SYSTEM_FILES:
        try:
            st = dev.stat(p)
        except LinkError as e:
            warn("skipped {}: {}".format(p, e))
            skipped += 1
            continue
        if st is not None and st[0] == "f":
            paths.append((p, st[1]))
        else:
            log("  {} is not on the device; nothing to keep".format(p))
    for top in SYSTEM_TREES:
        try:
            st = dev.stat(top)
            if st is None or st[0] != "d":
                log("  {} is not on the device; nothing to keep".format(top))
                continue
            for d, ents in dev.walk(top):
                for f in ents:
                    if not f.is_dir:
                        paths.append((posixpath.join(d, f.name), f.size))
        except LinkError as e:
            warn("skipped {}: {}".format(top, e))
            skipped += 1
    size = 0
    for p, sz in paths:
        data = fetch(p, sz)
        if data is None:
            skipped += 1
            continue
        store.write("system" + p, data)
        entries.append({"path": p, "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
        size += len(data)
        log("  {}".format(p))
    if entries:
        warn(SENSITIVE_NOTE)
    return len(entries), size, skipped


def system_paths(src):
    """The system files a backup holds, as device paths (empty for one made
    without --with-system)."""
    store = _store(src, False)
    try:
        return _system_in(store.names())
    finally:
        store.close()


def _system_in(names):
    return [n[len("system"):] for n in names
            if n.startswith("system/") and not n.endswith("/")]


def restore(dev, src, packages=True, dry_run=False, log=None, warn=None, progress=None,
            system=False, confirm=None, restart=True):
    """Put a backup back. Returns (files, failed).

    System files (a backup made --with-system) are written only when `system`
    is true and `confirm(notice)` agrees, last, with sudo, and then the device
    is restarted without saving (restart_unsaved), since otherwise the running
    OS writes its own settings back over them."""
    log = log or (lambda s: None)
    warn = warn or log
    store = _store(src, False)
    try:
        names = store.names()
        try:
            manifest = json.loads(store.read(MANIFEST).decode("utf-8"))
        except (KeyError, OSError, ValueError):
            manifest = {}
        files = [n[len("files"):] for n in names if n.startswith("files/") and not n.endswith("/")]
        dirs = set(manifest.get("dirs", []))
        dirs.update(n[len("files"):].rstrip("/") for n in names
                    if n.startswith("files/") and n.endswith("/"))
        dirs.discard("")
        pkgs = [n for n in names if n.startswith("packages/") and n.endswith(".app")]
        sysfiles = _system_in(names)
        if not files and not pkgs and not dirs and not sysfiles:
            raise RpcError("{} holds nothing to restore".format(src))

        made = set()
        failed = 0

        def ensure(d):
            if d in ("", "/") or d in made:
                return
            if not dry_run:
                dev.mkdir(d, parents=True)
            made.add(d)

        for d in sorted(dirs):
            ensure(d)
        done = 0
        for path in files:
            ensure(posixpath.dirname(path))
            data = store.read("files" + path)
            if dry_run:
                log("  would put {} ({} bytes)".format(path, len(data)))
                done += 1
                continue
            try:
                dev.put(data, path, progress(path, len(data)) if progress else None)
                done += 1
                log("  {}".format(path))
            except LinkError as e:
                failed += 1
                warn("could not restore {}: {}".format(path, e))
        if packages:
            for n in pkgs:
                name = n[len("packages/"):]
                if dry_run:
                    log("  would install {}".format(name))
                    continue
                r = install_bytes(dev, store.read(n), name)
                if r.rc == 0:
                    done += 1
                    log("  installed {}".format(name))
                else:
                    failed += 1
                    warn("could not install {}:\n{}".format(name, r.text.rstrip()))

        if sysfiles and not system:
            warn("the backup holds system files ({}); they were left as they are".format(
                ", ".join(sysfiles)))
        elif sysfiles and dry_run:
            for p in sysfiles:
                log("  would replace {}".format(p))
        elif sysfiles:
            if confirm is None or not confirm(system_restore_notice(sysfiles)):
                warn("system files left as they are")
            else:
                n, bad = _restore_system(dev, store, sysfiles, log, warn, progress)
                done += n
                failed += bad
                if n and restart and not bad:
                    log("Restarting the device without saving, so the restored settings stay.")
                    restart_unsaved(dev)
                elif bad:
                    warn("not restarting: some system files failed, and a restart now could "
                         "leave the device with half of them")
        return done, failed
    finally:
        store.close()


def _restore_system(dev, store, sysfiles, log, warn, progress):
    # Through sudo (AUTH): PUT into /os needs it. The directories the SSH files
    # sit in are made first; /os itself always exists.
    done = failed = 0
    for p in sysfiles:
        d = posixpath.dirname(p)
        if d not in ("/", "/os"):
            dev.mkdir(d, parents=True)
        data = store.read("system" + p)
        targets = [p] + ([SHADOWS[p]] if p in SHADOWS else [])
        try:
            for t in targets:
                dev.put(data, t, progress(t, len(data)) if progress else None)
            done += 1
            log("  {}".format(p))
        except LinkError as e:
            failed += 1
            warn("could not restore {}: {}".format(p, e))
    return done, failed
