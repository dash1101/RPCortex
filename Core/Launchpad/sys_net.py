# Desc: Network shell commands (wget, curl, runurl, ping, nslookup) - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_net.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi, inpt


def _tokenize(s):
    """Split a string on whitespace, respecting single/double quotes.

    Quote characters are removed; an empty quoted token ('') is preserved.
    Lets curl flags like  -H 'Auth: x'  and  -d '{"k":1}'  parse as one token.
    """
    out, cur = [], []
    in_q, q, started = False, None, False
    for ch in s:
        if ch in ('"', "'"):
            if not in_q:
                in_q, q, started = True, ch, True
            elif ch == q:
                in_q, q = False, None
            else:
                cur.append(ch); started = True
        elif ch in (' ', '\t') and not in_q:
            if started:
                out.append(''.join(cur)); cur = []; started = False
        else:
            cur.append(ch); started = True
    if started:
        out.append(''.join(cur))
    return out


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

    from net import wget as _wget, is_available, online
    if not is_available():
        error("WiFi not available on this board.")
        return
    if not online():
        error("Not connected to WiFi. Run: wifi connect")
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
        warn("Usage: runurl <url> [--keep] [-y]")
        return
    parts = args.strip().split()
    url  = parts[0]
    keep = '--keep' in parts
    yes  = '-y' in parts or '--yes' in parts

    from net import run_url, is_available, online
    if not is_available():
        error("WiFi not available on this board.")
        return
    if not online():
        error("Not connected to WiFi. Run: wifi connect")
        return

    # Code-execution guard: runurl downloads a .py and runs it with FULL device
    # access. Make that an explicit, acknowledged act so a stray startup task or
    # .rps line can't silently pull and run remote code. '-y' bypasses for
    # trusted automation.
    if not yes:
        warn("This downloads and RUNS code from:")
        multi("  " + url)
        warn("Only continue if you trust this source — it gets full device access.")
        if inpt("Run it? (yes/no)").strip().lower() not in ('y', 'yes'):
            info("Cancelled.")
            return
    try:
        run_url(url, keep=keep)
    except Exception as e:
        error("runurl failed: {}".format(e))


def _parse_curl_args(tokens):
    """Parse curl flags into (url, kwargs). Returns (None, None) on error.

    Supports: -v  -s/--silent  -I/--head  -X <method>  -d <data>
              -H <header>  -o <file>  --timeout <secs>
    """
    url     = None
    kwargs  = {'verbose': False, 'silent': False, 'head_only': False,
               'method': 'GET', 'data': None, 'headers': None,
               'output': None, 'timeout': 15}
    i = 0
    n = len(tokens)
    while i < n:
        t = tokens[i]
        if t == '-v':
            kwargs['verbose'] = True
        elif t in ('-s', '--silent'):
            kwargs['silent'] = True
        elif t in ('-I', '--head'):
            kwargs['head_only'] = True
        elif t == '-X' and i + 1 < n:
            kwargs['method'] = tokens[i + 1].upper(); i += 1
        elif t == '-d' and i + 1 < n:
            kwargs['data'] = tokens[i + 1]
            if kwargs['method'] == 'GET':
                kwargs['method'] = 'POST'
            i += 1
        elif t == '-H' and i + 1 < n:
            hdr = tokens[i + 1]
            if ':' in hdr:
                k, v = hdr.split(':', 1)
                if kwargs['headers'] is None:
                    kwargs['headers'] = {}
                kwargs['headers'][k.strip()] = v.strip()
            i += 1
        elif t == '-o' and i + 1 < n:
            kwargs['output'] = tokens[i + 1]; i += 1
        elif t == '--timeout' and i + 1 < n:
            try:
                kwargs['timeout'] = int(tokens[i + 1])
            except ValueError:
                pass
            i += 1
        elif t.startswith('-'):
            error("Unknown curl flag: {}".format(t))
            return None, None
        elif url is None:
            url = t
        i += 1
    return url, kwargs


def curl(args=None):
    if not args:
        warn("Usage: curl <url> [-v] [-s] [-I] [-X M] [-d data] [-H 'K: V'] [-o file] [--timeout n]")
        return
    # Tokenize, honouring single/double quotes so -H 'Auth: x' and -d '{...}' work.
    tokens = _tokenize(args.strip())
    url, kwargs = _parse_curl_args(tokens)
    if url is None:
        if kwargs is not None:
            error("No URL given.")
        return

    # Resolve a relative -o path against the current directory.
    if kwargs.get('output') and not kwargs['output'].startswith('/'):
        kwargs['output'] = uos.getcwd().rstrip('/') + '/' + kwargs['output']

    from net import is_available, online
    if not is_available():
        error("WiFi not available on this board.")
        return
    if not online():
        error("Not connected to WiFi. Run: wifi connect")
        return
    import gc
    try:
        _cmd_cache.clear()
    except NameError:
        pass
    gc.collect()
    import net
    try:
        net.curl(url, **kwargs)
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
    from net import ping as _ping, is_available, online
    if not is_available():
        error("WiFi not available on this board.")
        return
    if not online():
        error("Not connected to WiFi. Run: wifi connect")
        return
    _ping(host, count=count)


def nslookup(args=None):
    if not args:
        warn("Usage: nslookup <host>")
        return
    from net import nslookup as _nslookup, is_available, online
    if not is_available():
        error("WiFi not available on this board.")
        return
    if not online():
        error("Not connected to WiFi. Run: wifi connect")
        return
    _nslookup(args.strip())
