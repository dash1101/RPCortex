"""Backing up a device's files to the computer, and putting them back.

What is kept: everything a person put on the device. That is /home, /etc,
/usb and anything else at the top level, but not /os (the system's own
files: settings, accounts, the device key, firmware images), /tmp (emptied at
every boot) or /sd (a card, which is read-only here). Installed packages,
which live in /os/pkg, are added with --with-packages; restoring them installs
them again with `pkg install`.

A backup is a directory or a .zip with the same layout:

    rpc-backup.json         what was backed up, from which device, with digests
    files/<path>            each file, at its path on the device
    packages/<name>.app     installed packages (only with --with-packages)
"""
import datetime
import hashlib
import json
import os
import posixpath
import zipfile

from .errors import LinkError, RpcError
from .pkg import install_bytes

MANIFEST = "rpc-backup.json"
SKIP_TOP = ("os", "tmp", "sd")


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


def backup(dev, dest, with_packages=False, log=None, warn=None, progress=None):
    """Copy the device's files into `dest` (a directory, or a path ending .zip).
    Returns (files, bytes, skipped)."""
    log = log or (lambda s: None)
    warn = warn or log
    manifest = {"format": 1,
                "created": datetime.datetime.now().replace(microsecond=0).isoformat(),
                "device": dev.hello, "files": [], "dirs": [], "packages": []}
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

        store.write(MANIFEST, json.dumps(manifest, indent=2).encode("utf-8"))
    finally:
        store.close()
    return len(manifest["files"]) + len(manifest["packages"]), total, skipped


def restore(dev, src, packages=True, dry_run=False, log=None, warn=None, progress=None):
    """Put a backup back. Returns (files, failed)."""
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
        if not files and not pkgs and not dirs:
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
        return done, failed
    finally:
        store.close()
