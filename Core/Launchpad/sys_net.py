# Desc: Network shell commands (wget, curl, runurl, ping, nslookup) - RPCortex Nebula OS
# File: /Core/Launchpad/sys_net.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta4

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi


def wget(args=None):
    if not args:
        warn("Usage: wget <url> [destination]")
        return
    parts = args.strip().split(None, 1)
    url  = parts[0]
    dest = parts[1].strip() if len(parts) > 1 else None

    if dest and not dest.startswith('/'):
        dest = uos.getcwd().rstrip('/') + '/' + dest
    elif not dest:
        fname = url.rstrip('/').split('/')[-1] or 'download'
        dest = uos.getcwd().rstrip('/') + '/' + fname

    from net import wget as _wget, is_available
    if not is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return
    import gc
    try:
        _cmd_cache.clear()
    except NameError:
        pass
    gc.collect()
    try:
        status_code, written = _wget(url, dest=dest, verbose=True)
        if status_code == 200:
            ok("Saved {} bytes to '{}'".format(written, dest))
        else:
            error("HTTP {} — file may be incomplete.".format(status_code))
    except MemoryError as e:
        error("Not enough RAM: {}".format(e))
        info("Tip: run 'freeup' to reclaim memory, then retry.")
    except Exception as e:
        error("Download failed: {}".format(e))


def runurl(args=None):
    if not args:
        warn("Usage: runurl <url> [--keep]")
        return
    parts = args.strip().split()
    url  = parts[0]
    keep = '--keep' in parts

    from net import run_url, is_available
    if not is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return
    try:
        run_url(url, keep=keep)
    except Exception as e:
        error("runurl failed: {}".format(e))


def curl(args=None):
    if not args:
        warn("Usage: curl <url> [-v]")
        return
    parts = args.strip().split()
    url     = parts[0]
    verbose = '-v' in parts

    from net import is_available
    if not is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return
    import gc
    try:
        _cmd_cache.clear()
    except NameError:
        pass
    gc.collect()
    import net
    try:
        net.curl(url, verbose=verbose)  # streams directly to stdout
    except MemoryError as e:
        error("Not enough RAM: {}".format(e))
        info("Tip: run 'freeup' to reclaim memory, then retry.")
    except Exception as e:
        error("curl failed: {}".format(e))


def ping(args=None):
    if not args:
        warn("Usage: ping <host> [count]")
        return
    parts = args.strip().split(None, 1)
    host  = parts[0]
    count = 4
    if len(parts) > 1:
        try:
            count = int(parts[1])
        except ValueError:
            warn("Invalid count — defaulting to 4.")
    from net import ping as _ping, is_available
    if not is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return
    _ping(host, count=count)


def nslookup(args=None):
    if not args:
        warn("Usage: nslookup <host>")
        return
    from net import nslookup as _nslookup, is_available
    if not is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return
    _nslookup(args.strip())
