# Desc: RPCortex OS update installer — extracts .rpc archives to the device filesystem
# File: /Core/rpc_install.py
# Last Updated: 3/26/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta3
# Author: dash1101
#
# .rpc files are standard ZIP archives (renamed).  Used by:
#   - `update from-file` shell command  (Core/Launchpad/sys_sys.py)
#   - `reinstall` command stages a .rpc for the boot-time stub
#
# Design:
#   Central Directory approach — reads EOCD to find the CD, then iterates CD
#   entries for accurate comp_size values.  This fixes data-descriptor ZIPs
#   (general purpose bit 3 set) where comp_size=0 in the Local File Header.
#   After CD parse, seeks directly to each wanted entry for extraction.
#   Peak RAM ~ largest_single_decompressed_file, not archive_size.

import uos
import gc
import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import ok, warn, error, info

# ---------------------------------------------------------------------------
# File filter
# ---------------------------------------------------------------------------

_SKIP = (
    'website/', 'repo/', '.git', '__pycache__', 'CLAUDE',
    'Users/', 'Nebula/', 'temp/', 'tests/',
)
_EXTS = ('.py', '.cfg', '.lp')

def _want(rel):
    """True if this archive-relative path should be written to the device."""
    for s in _SKIP:
        if rel.startswith(s) or ('/' + s) in rel:
            return False
    # programs.lp contains user-installed package command entries — preserve
    # it during OS updates so installed packages keep working after a reboot.
    # (rpc_stub.py DOES write it on fresh installs since there's nothing to lose.)
    if rel == 'programs.lp' or rel.endswith('/programs.lp'):
        return False
    for e in _EXTS:
        if rel.endswith(e):
            return True
    return rel == 'main.py'

# ---------------------------------------------------------------------------
# Filesystem helpers
# ---------------------------------------------------------------------------

def _makedirs(path):
    parts = [p for p in path.split('/') if p]
    cur = ''
    for part in parts:
        cur += '/' + part
        try:
            uos.mkdir(cur)
        except OSError:
            pass

# ---------------------------------------------------------------------------
# ZIP helpers
# ---------------------------------------------------------------------------

def _u32(b):
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)

def _u16(b):
    return b[0] | (b[1] << 8)

def _fskip(fh, n):
    """Skip n bytes in file handle without a large buffer allocation."""
    if n <= 0:
        return
    try:
        fh.seek(n, 1)
    except Exception:
        while n > 0:
            chunk = min(n, 256)
            fh.read(chunk)
            n -= chunk

# ---------------------------------------------------------------------------
# Main installer
# ---------------------------------------------------------------------------

def install_rpc(archive_path):
    """
    Extract a .rpc archive to the device filesystem.

    Uses the ZIP Central Directory for accurate compressed sizes — works even
    when Local File Headers carry comp_size=0 (data descriptor flag set),
    which is common in ZIPs produced by GitHub Actions or Windows Explorer.

    Returns (n_installed, success_bool).
    """
    gc.collect()
    info("Reading archive: {}".format(archive_path), p="Update")

    try:
        file_size = uos.stat(archive_path)[6]
        fh = open(archive_path, 'rb')
    except OSError as e:
        error("Cannot open '{}': {}".format(archive_path, e), p="Update")
        return 0, False

    # ── Locate End of Central Directory (EOCD) ────────────────────
    # Search last 512 bytes for PK\x05\x06.  Handles ZIP comments up
    # to ~490 bytes; our .rpc files are generated without comments.
    try:
        search_off = max(0, file_size - 512)
        fh.seek(search_off)
        tail = fh.read(file_size - search_off)
        eocd_pos = tail.rfind(b'PK\x05\x06')
        if eocd_pos == -1:
            raise ValueError("EOCD signature not found — not a valid ZIP")
        eocd = tail[eocd_pos:eocd_pos + 22]
        if len(eocd) < 22:
            raise ValueError("EOCD truncated")
        del tail
    except Exception as e:
        error("ZIP structure error: {}".format(e), p="Update")
        fh.close()
        return 0, False

    cd_count = _u16(eocd[10:12])
    cd_off   = _u32(eocd[16:20])
    del eocd
    gc.collect()

    info("{} entries in archive.".format(cd_count), p="Update")

    # ── Parse Central Directory ────────────────────────────────────
    # CDR always has the correct comp_size — not affected by data descriptors.
    # Layout after 4-byte sig: 42 bytes of fixed fields, then fname, extra, comment.
    #   cdr[6:8]   = compression method
    #   cdr[16:20] = compressed size
    #   cdr[24:26] = filename length
    #   cdr[26:28] = extra field length
    #   cdr[28:30] = file comment length
    #   cdr[38:42] = local header offset
    entries = []   # (fname, comp_method, comp_size, local_off)
    try:
        fh.seek(cd_off)
        for _ in range(cd_count):
            sig = fh.read(4)
            if sig != b'PK\x01\x02':
                break
            cdr = fh.read(42)
            if len(cdr) < 42:
                break
            comp_method = _u16(cdr[6:8])
            comp_size   = _u32(cdr[16:20])
            fname_len   = _u16(cdr[24:26])
            extra_len   = _u16(cdr[26:28])
            comment_len = _u16(cdr[28:30])
            local_off   = _u32(cdr[38:42])
            try:
                fname = fh.read(fname_len).decode('utf-8')
            except Exception:
                fh.read(fname_len)
                fname = ''
            _fskip(fh, extra_len + comment_len)
            entries.append((fname, comp_method, comp_size, local_off))
    except Exception as e:
        error("CD parse error: {}".format(e), p="Update")
        fh.close()
        return 0, False

    gc.collect()

    # ── Detect top-level prefix ────────────────────────────────────
    # Only strip a prefix if EVERY file in the archive starts with it.
    # Handles GitHub-style ZIPs (RPCortex-Tag/Core/…) and flat ZIPs.
    names = [e[0] for e in entries if e[0] and not e[0].endswith('/')]
    prefix = ''
    if names:
        slash = names[0].find('/')
        if slash > 0:
            cand = names[0][:slash + 1]
            if all(n.startswith(cand) for n in names):
                prefix = cand
    del names
    gc.collect()

    if prefix:
        info("Stripping prefix: '{}'.".format(prefix), p="Update")

    # ── Extract wanted files ───────────────────────────────────────
    info("Preserving: /Users/  /Nebula/  programs.lp (user packages)", p="Update")

    n_ok   = 0
    n_skip = 0
    n_fail = 0
    n_dir  = 0

    for (fname, comp_method, comp_size, local_off) in entries:
        rel = fname[len(prefix):] if (prefix and fname.startswith(prefix)) else fname

        if not rel:
            continue

        if rel.endswith('/'):
            n_dir += 1
            continue

        if not _want(rel):
            n_skip += 1
            continue

        # Seek to the Local File Header to find the exact data offset.
        # We use local fname_len + extra_len to skip to the data; we
        # use comp_size from the CDR (which is always accurate).
        try:
            fh.seek(local_off)
            lfh = fh.read(30)
            if len(lfh) < 30 or lfh[:4] != b'PK\x03\x04':
                warn("  Bad LFH for '{}' — skipping.".format(rel), p="Update")
                n_fail += 1
                continue
            local_fname_len = _u16(lfh[26:28])
            local_extra_len = _u16(lfh[28:30])
            _fskip(fh, local_fname_len + local_extra_len)
        except Exception as e:
            warn("  Seek '{}': {}".format(rel, e), p="Update")
            n_fail += 1
            continue

        # Read compressed data using CD's comp_size (always accurate)
        raw = fh.read(comp_size)

        # Decompress
        file_data = None
        if comp_method == 0:
            file_data = raw

        elif comp_method == 8:
            try:
                import zlib
                file_data = zlib.decompress(raw, -15)
            except ImportError:
                pass
            except Exception as e:
                warn("  zlib '{}': {}".format(rel, e), p="Update")

            if file_data is None:
                try:
                    import uzlib
                    file_data = uzlib.decompress(raw, -15)
                except ImportError:
                    pass
                except Exception as e:
                    warn("  uzlib '{}': {}".format(rel, e), p="Update")

            if file_data is None:
                warn("  No decompressor for '{}' — skipping.".format(rel), p="Update")
                raw = None
                n_fail += 1
                gc.collect()
                continue

        else:
            warn("  Compression {} unsupported ('{}').".format(comp_method, rel), p="Update")
            raw = None
            n_skip += 1
            gc.collect()
            continue

        raw = None
        gc.collect()

        # Write to device
        device_path = '/' + rel
        parent = '/'.join(device_path.split('/')[:-1])
        if parent:
            _makedirs(parent)

        try:
            with open(device_path, 'wb') as out:
                out.write(file_data)
            info("  + {}".format(device_path), p="Update")
            n_ok += 1
        except OSError as e:
            warn("  Write failed '{}': {}".format(device_path, e), p="Update")
            n_fail += 1

        file_data = None
        gc.collect()

    try:
        fh.close()
    except Exception:
        pass

    gc.collect()
    ok("{} installed, {} skipped, {} dir entries, {} failed.".format(
        n_ok, n_skip, n_dir, n_fail), p="Update")
    return n_ok, n_ok > 0
