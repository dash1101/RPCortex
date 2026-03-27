# Desc: Reinstall boot stub for RPCortex — copied to /main.py by `reinstall`
# File: /Core/rpc_stub.py
# Last Updated: 3/27/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta2
# Author: dash1101
#
# ─────────────────────────────────────────────────────────────────────────
# This file is copied to /main.py by the `reinstall` shell command.
# It runs on the NEXT BOOT after a full system wipe.
#
# Behaviour:
#   - Looks for /update.rpc on the filesystem.
#   - If found: extracts it (self-contained, no Core/ needed), reboots.
#   - If not found: prints instructions and leaves MicroPython REPL open.
#
# Central Directory approach — reads EOCD to find CD, parses CD entries
# for accurate comp_size values.  Fixes data-descriptor ZIPs where
# comp_size=0 in the Local File Header.
# Peak RAM ~ largest_single_decompressed_file, not archive_size.
# ─────────────────────────────────────────────────────────────────────────

import uos
import gc
import machine

_R   = "\033[0m"
_CYN = "\033[96m"
_YEL = "\033[93m"
_ERR = "\033[91m"
_GRY = "\033[90m"

def _p(color, text):
    print(color + text + _R)

RPC_PATH = '/update.rpc'

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
# ZIP helpers (self-contained — no imports from Core/)
# ---------------------------------------------------------------------------

def _u32(b):
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)

def _u16(b):
    return b[0] | (b[1] << 8)

def _fskip(fh, n):
    """Skip n bytes without a large buffer."""
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
# File filter — same rules as rpc_install.py
# ---------------------------------------------------------------------------

_SKIP = ('website/', 'repo/', '.git', '__pycache__', 'CLAUDE', 'temp/', 'tests/')
# Note: Users/ and Nebula/ are NOT skipped here — this stub runs after a full
# wipe so there is no existing user data to preserve.  If the .rpc includes
# default Nebula/ content (e.g. a pre-seeded registry), it will be installed.
_EXTS = ('.py', '.cfg', '.lp')

def _want(rel):
    for s in _SKIP:
        if rel.startswith(s) or ('/' + s) in rel:
            return False
    for e in _EXTS:
        if rel.endswith(e):
            return True
    return rel == 'main.py'

# ---------------------------------------------------------------------------
# Self-contained .rpc extractor (Central Directory approach)
# ---------------------------------------------------------------------------

def _install(path):
    _p(_CYN, '[:] Opening archive: ' + path)
    try:
        file_size = uos.stat(path)[6]
        fh = open(path, 'rb')
    except Exception as e:
        _p(_ERR, '[!] Cannot open archive: ' + str(e))
        return False

    # ── Find EOCD ─────────────────────────────────────────────────
    try:
        search_off = max(0, file_size - 512)
        fh.seek(search_off)
        tail = fh.read(file_size - search_off)
        eocd_pos = tail.rfind(b'PK\x05\x06')
        if eocd_pos == -1:
            raise ValueError('EOCD not found')
        eocd = tail[eocd_pos:eocd_pos + 22]
        if len(eocd) < 22:
            raise ValueError('EOCD truncated')
        del tail
    except Exception as e:
        _p(_ERR, '[!] ZIP structure error: ' + str(e))
        fh.close()
        return False

    cd_count = _u16(eocd[10:12])
    cd_off   = _u32(eocd[16:20])
    del eocd
    gc.collect()

    _p(_CYN, '[:] {} entries in archive.'.format(cd_count))

    # ── Parse Central Directory ────────────────────────────────────
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
        _p(_ERR, '[!] CD parse error: ' + str(e))
        fh.close()
        return False

    gc.collect()

    # ── Detect prefix ─────────────────────────────────────────────
    names = [e[0] for e in entries if e[0] and not e[0].endswith('/')]
    prefix = ''
    if names:
        s = names[0].find('/')
        if s > 0:
            cand = names[0][:s + 1]
            if all(n.startswith(cand) for n in names):
                prefix = cand
    del names
    gc.collect()

    _p(_CYN, '[:] Prefix: ' + ("'" + prefix + "'" if prefix else '(none)'))

    # ── Extract ───────────────────────────────────────────────────
    count = 0
    skipped = 0

    for (fname, comp_method, comp_size, local_off) in entries:
        rel = fname[len(prefix):] if (prefix and fname.startswith(prefix)) else fname

        if not rel or rel.endswith('/'):
            continue

        if not _want(rel):
            skipped += 1
            continue

        # Seek to data — skip LFH using local fname/extra lengths
        try:
            fh.seek(local_off)
            lfh = fh.read(30)
            if len(lfh) < 30 or lfh[:4] != b'PK\x03\x04':
                print('[?] Bad LFH: ' + rel)
                continue
            local_fname_len = _u16(lfh[26:28])
            local_extra_len = _u16(lfh[28:30])
            _fskip(fh, local_fname_len + local_extra_len)
        except Exception as e:
            print('[?] Seek failed: ' + rel + '  ' + str(e))
            continue

        # Read using CD's comp_size (always accurate)
        raw = fh.read(comp_size)

        fd = None
        if comp_method == 0:
            fd = raw
        elif comp_method == 8:
            try:
                import zlib
                fd = zlib.decompress(raw, -15)
            except Exception:
                pass
            if fd is None:
                try:
                    import uzlib
                    fd = uzlib.decompress(raw, -15)
                except Exception:
                    pass
            if fd is None:
                print('[?] Cannot decompress: ' + rel)
                raw = None
                gc.collect()
                continue
        else:
            raw = None
            gc.collect()
            continue

        raw = None
        gc.collect()

        dp = '/' + rel
        parent = '/'.join(dp.split('/')[:-1])
        if parent:
            _makedirs(parent)

        try:
            with open(dp, 'wb') as f:
                f.write(fd)
            print('  + ' + dp)
            count += 1
        except Exception as e:
            print('[?] Write failed: ' + dp + '  ' + str(e))

        fd = None
        gc.collect()

    try:
        fh.close()
    except Exception:
        pass

    gc.collect()
    _p(_CYN, '[@] Installed {} file(s), {} skipped.'.format(count, skipped))
    return count > 0

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

print()
print(_CYN + '  \u250c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510' + _R)
print(_CYN + '  \u2502    RPCortex \u2014 Reinstall Stub           \u2502' + _R)
print(_CYN + '  \u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518' + _R)
_p(_YEL,  '  The OS has been wiped. Reinstallation required.')
print()

try:
    uos.stat(RPC_PATH)
    _p(_CYN, '  Found ' + RPC_PATH + '.  Starting auto-install...')
    print()
    if _install(RPC_PATH):
        _p(_CYN, '')
        _p(_CYN, '[@] Installation complete.  Cleaning up...')
        try:
            uos.remove(RPC_PATH)
        except Exception:
            pass
        _p(_CYN, '[:] Rebooting in 3 seconds...')
        import utime
        utime.sleep(3)
        machine.reset()
    else:
        _p(_ERR, '[!] Auto-install failed.')
        print('  Check the archive and try again, or use the Web Installer.')
        print()
        print('  Web Installer:  rpc.novalabs.app/install.html')

except OSError:
    print('  No ' + RPC_PATH + ' found on device.')
    print()
    print(_GRY + '  \u2500\u2500\u2500 Reinstallation options \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500' + _R)
    print()
    print('  Option 1  \u2014  Transfer a .rpc file, then reboot:')
    print()
    print('    a) Connect via Web Installer, use the File Transfer tool:')
    print('         rpc.novalabs.app/install.html')
    print('         Send the .rpc file to /update.rpc')
    print()
    print('    b) Reboot \u2014 the stub will detect and install it automatically.')
    print()
    print('  Option 2  \u2014  Full reinstall via Web Installer:')
    print()
    print('    Open:  rpc.novalabs.app/install.html')
    print('    Connect your device via USB and click "Install Now".')
    print()
    _p(_GRY, '  MicroPython REPL is available.  The Web Installer works here.')
    print()
