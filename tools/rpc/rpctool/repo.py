"""The package repository, read on the computer instead of on the device.

The same index the device's `pkg` reads (os/shell/pkgrepo.cpp), the same rule
for which architecture runs where, and the same check: a package is installed
only when its bytes match the SHA-256 the index publishes. Downloading here
means a device with no network can still install by name.
"""
import hashlib
import json
import os
import posixpath

try:
    from urllib.parse import urljoin, urlparse, unquote
    from urllib.request import Request, urlopen
except ImportError:  # pragma: no cover
    raise

from .errors import DigestMismatch, RpcError

# REPO_DEFAULT in os/shell/pkgrepo.cpp.
REPO_DEFAULT = "https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo-v2/index.json"

# PICO_BOARD -> the architecture its image runs (MY_ARCH in pkgrepo.cpp).
BOARD_ARCH = {
    "pico2_w": "armv8m",
    "pico2": "armv8m",
    "pico_w": "armv6m",
    "pico": "armv6m",
}

USER_AGENT = "rpctool/1.0 (+RPCortex host tools)"


def board_arch(board):
    """armv8m for RP2350 boards, armv6m for RP2040 ones, None when unknown."""
    if not board:
        return None
    b = board.lower()
    if b in BOARD_ARCH:
        return BOARD_ARCH[b]
    if "rp2350" in b or b.startswith("pico2"):
        return "armv8m"
    if "rp2040" in b or b.startswith("pico"):
        return "armv6m"
    return None


def arch_runs_here(arch, mine):
    """arch_runs_here() in pkgrepo.cpp: a comma-separated list, unstated means
    allowed, and ARMv8-M Baseline runs ARMv6-M code but not the reverse."""
    if not arch or not arch.strip():
        return True
    for one in arch.split(","):
        one = one.strip()
        if not one:
            continue
        if one == mine:
            return True
        if mine == "armv8m" and one == "armv6m":
            return True
    return False


class Package(object):
    FIELDS = ("name", "ver", "desc", "author", "url", "sha256", "abi", "arch", "kind", "size")

    def __init__(self, d, base):
        self.name = str(d.get("name", ""))
        self.ver = str(d.get("ver", d.get("version", "")))
        self.desc = str(d.get("desc", ""))
        self.author = str(d.get("author", ""))
        self.url = str(d.get("url", ""))
        self.sha256 = str(d.get("sha256", "")).lower()
        self.abi = str(d.get("abi", ""))
        self.arch = str(d.get("arch", ""))
        self.kind = str(d.get("kind", ""))
        try:
            self.size = int(d.get("size", 0) or 0)
        except (TypeError, ValueError):
            self.size = 0
        self.base = base

    @property
    def location(self):
        """The blob's URL, or a local path when the index was a local file."""
        u = self.url
        if urlparse(u).scheme in ("http", "https", "file"):
            return u
        if self.base and urlparse(self.base).scheme in ("http", "https"):
            return urljoin(self.base, u)
        if self.base:
            return os.path.join(os.path.dirname(self.base), u)
        return u

    def tag(self):
        k = self.kind.lower()
        if k.startswith("t") or k == "diagnostic":
            return "test"
        if k.startswith("e"):
            return "example"
        return ""


def _read(source, progress=None):
    if urlparse(source).scheme in ("http", "https", "file"):
        return fetch(source, progress)
    with open(source, "rb") as f:
        return f.read()


def load_index(source=None):
    """Packages listed by an index URL or file. Entries with no name or URL
    are skipped, as the device skips them."""
    source = source or REPO_DEFAULT
    try:
        doc = json.loads(_read(source).decode("utf-8"))
    except ValueError as e:
        raise RpcError("{} is not a package index: {}".format(source, e))
    pkgs = doc.get("packages") if isinstance(doc, dict) else None
    if not isinstance(pkgs, list):
        raise RpcError("{} has no package list".format(source))
    out = []
    for d in pkgs:
        if isinstance(d, dict):
            p = Package(d, source)
            if p.name and p.url:
                out.append(p)
    return out


def choose(packages, name, arch):
    """The entry to install for `name` on a board running `arch`.

    Names match case-insensitively, as on the device. Where an index lists one
    name for several architectures, an entry built for this one is preferred,
    then any entry that runs here."""
    same = [p for p in packages if p.name.lower() == name.lower()]
    if not same:
        raise RpcError("No package called '{}'. 'rpc install --list' shows what there is."
                       .format(name))
    if arch is None:
        return same[0]
    exact = [p for p in same if arch in [a.strip() for a in p.arch.split(",")]]
    runs = [p for p in same if arch_runs_here(p.arch, arch)]
    if exact:
        return exact[0]
    if runs:
        return runs[0]
    raise RpcError("{} is built for {}; this board is {}.".format(same[0].name, same[0].arch, arch))


def fetch(url, progress=None, limit=16 * 1024 * 1024):
    """Download over HTTP(S). Certificates are verified with the system store."""
    req = Request(url, headers={"User-Agent": USER_AGENT})
    try:
        resp = urlopen(req, timeout=30)
    except Exception as e:
        raise RpcError("could not fetch {}: {}{}".format(url, e, _cert_hint(e)))
    with resp:
        try:
            total = int(resp.headers.get("Content-Length") or 0)
        except ValueError:
            total = 0
        buf = bytearray()
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            buf.extend(chunk)
            if len(buf) > limit:
                raise RpcError("{} is larger than {} bytes; refusing".format(url, limit))
            if progress:
                progress(len(buf), total or len(buf))
    return bytes(buf)


def _cert_hint(e):
    if "CERTIFICATE_VERIFY_FAILED" in str(e):
        return ("\n  Python cannot verify HTTPS here. On macOS run 'Install Certificates.command' "
                "from the Python folder in Applications.")
    return ""


def download(pkg, progress=None):
    """The package's bytes, verified against the index. Returns (data, verified)."""
    data = _read(pkg.location, progress)
    if pkg.sha256:
        have = hashlib.sha256(data).hexdigest()
        if have != pkg.sha256:
            raise DigestMismatch(
                "Checksum mismatch: the download does not match the index.\n"
                "  expected  {}\n  received  {}\n  Nothing was installed.".format(pkg.sha256, have))
        return data, True
    return data, False


def url_basename(url):
    path = unquote(urlparse(url).path)
    return posixpath.basename(path.rstrip("/")) or "download"
