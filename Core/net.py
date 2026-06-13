# Desc: WiFi networking and HTTP client for RPCortex - Pulsar OS
# File: /Core/net.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
# Author: dash1101
#
# Supported hardware:
#   - Raspberry Pi Pico W (RP2040 + CYW43439)
#   - Raspberry Pi Pico 2 W (RP2350 + CYW43439)
#   - ESP32 / ESP32-S2 / ESP32-S3 (any MicroPython build with 'network' module)
#
# All network calls use the standard MicroPython 'network' module, so the same
# code runs on both Pico W and ESP32 without changes.
#
# NOTE: WiFi passwords are stored in plaintext in registry.cfg under [Networks].
#       This is a known limitation.  Do not store sensitive credentials.

import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import ok, warn, error, info, multi, spin, spin_done

# Saved networks file — one "ssid\tpassword" per line (unlimited entries).
# Replaces the old 2-slot registry approach (Networks.WiFi_SSID_1/2).
_NETWORKS_FILE = '/Pulsar/Registry/networks.cfg'

# ---------------------------------------------------------------------------
# Detection helpers
# ---------------------------------------------------------------------------

def is_available():
    """Return True if this board has WiFi hardware."""
    try:
        import network
        return hasattr(network, 'WLAN')
    except ImportError:
        return False


def online():
    """Return True if WiFi hardware is present AND the STA interface is connected."""
    try:
        import network
        if not hasattr(network, 'WLAN'):
            return False
        wlan = network.WLAN(network.STA_IF)
        return wlan.active() and wlan.isconnected()
    except Exception:
        return False


def _get_wlan():
    """Return the STA interface, or None if not supported."""
    try:
        import network
        return network.WLAN(network.STA_IF)
    except Exception:
        return None

# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

def status():
    """
    Return a dict describing current WiFi state.
    Keys: available, active, connected, ip, ssid
    """
    result = {
        'available': False,
        'active':    False,
        'connected': False,
        'ip':        None,
        'ssid':      None,
        'rssi':      None,
    }
    wlan = _get_wlan()
    if wlan is None:
        return result

    result['available'] = True
    result['active']    = wlan.active()

    if wlan.isconnected():
        result['connected'] = True
        cfg = wlan.ifconfig()
        result['ip'] = cfg[0]
        try:
            result['ssid'] = wlan.config('ssid')
        except Exception:
            result['ssid'] = '?'
        try:
            result['rssi'] = wlan.status('rssi')   # current-link signal (dBm)
        except Exception:
            result['rssi'] = None

    return result

# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------

def scan():
    """
    Scan for nearby WiFi networks.
    Returns a list of dicts: {ssid, rssi, channel, security, hidden}.
    Activates the interface temporarily if needed.
    """
    wlan = _get_wlan()
    if wlan is None:
        error("WiFi not supported on this board.")
        return []

    was_active = wlan.active()
    if not was_active:
        wlan.active(True)

    results = []
    try:
        raw = wlan.scan()
        _SEC = {0: 'open', 1: 'WEP', 2: 'WPA', 3: 'WPA2', 4: 'WPA/WPA2'}
        for entry in raw:
            # (ssid, bssid, channel, rssi, security, hidden)
            ssid     = entry[0].decode('utf-8', 'ignore') if isinstance(entry[0], bytes) else str(entry[0])
            channel  = entry[2]
            rssi     = entry[3]
            security = _SEC.get(entry[4], '?')
            hidden   = bool(entry[5]) if len(entry) > 5 else False
            results.append({
                'ssid': ssid,
                'rssi': rssi,
                'channel': channel,
                'security': security,
                'hidden': hidden,
            })
    except Exception as e:
        error("Scan failed: {}".format(e))
    finally:
        # Restore previous state
        if not was_active:
            wlan.active(False)

    # Sort by signal strength (strongest first)
    results.sort(key=lambda x: x['rssi'], reverse=True)
    return results

# ---------------------------------------------------------------------------
# Connect / disconnect
# ---------------------------------------------------------------------------

def connect(ssid, password, timeout=20, silent=False):
    """
    Connect to a specific WiFi network.
    silent=True: print nothing unless an error occurs.
    Returns True on success, False on failure/timeout.
    """
    import utime

    wlan = _get_wlan()
    if wlan is None:
        error("WiFi not supported on this board.")
        return False

    if wlan.isconnected():
        cur_ssid = '?'
        try:
            cur_ssid = wlan.config('ssid')
        except Exception:
            pass
        if cur_ssid == ssid:
            if not silent:
                ok("Already connected to '{}'.".format(ssid))
            return True
        wlan.disconnect()
        utime.sleep_ms(300)

    wlan.active(True)
    # On a cold boot the radio may not be fully up the instant we activate it;
    # issuing connect() too early silently fails (and a manual retry "right
    # after" then works). Wait briefly for the interface to come active first.
    _settle = utime.ticks_ms()
    while utime.ticks_diff(utime.ticks_ms(), _settle) < 2000:
        try:
            if wlan.active():
                break
        except Exception:
            break
        utime.sleep_ms(50)
    wlan.connect(ssid, password)

    start = utime.ticks_ms()
    deadline = utime.ticks_add(start, timeout * 1000)
    i = 0
    label = "Connecting to '{}'...".format(ssid)
    while not wlan.isconnected():
        if utime.ticks_diff(deadline, utime.ticks_ms()) <= 0:
            wlan.disconnect()
            if not silent:
                spin_done()
            error("Connection to '{}' timed out after {}s.".format(ssid, timeout))
            return False
        if not silent:
            spin(label, i, start)     # in-place "Connecting to 'X'... \ (3s)"
        utime.sleep_ms(250)
        i += 1

    cfg = wlan.ifconfig()
    if not silent:
        spin_done()
        ok("Connected to '{}'!  IP: {}  Gateway: {}".format(ssid, cfg[0], cfg[2]))

    # Auto-save on successful connection
    try:
        add_saved(ssid, password)
    except Exception:
        pass

    return True


def connect_saved(timeout=20, silent=False):
    """
    Attempt to connect to saved networks (tries each in order).
    Returns True if a connection was established.
    silent=True suppresses info messages; errors still print.
    """
    if online():           # already connected — nothing to do
        return True
    nets = _read_networks()
    if not nets:
        return False
    # Two passes: a cold-boot autoconnect occasionally fails on the first try
    # (radio just coming up) but succeeds immediately after — so retry the whole
    # saved-network list once before giving up.
    for _attempt in (1, 2):
        for ssid, pw in nets:
            # No chatty "Trying saved network" line — connect() shows a clean
            # in-place spinner ("Connecting to 'X'... \ (3s)") with the SSID.
            if connect(ssid, pw, timeout=timeout, silent=silent):
                return True
        if online():
            return True
    return False


def disconnect():
    """Disconnect from the current WiFi network."""
    wlan = _get_wlan()
    if wlan is None:
        error("WiFi not supported on this board.")
        return
    if not wlan.isconnected():
        warn("Not currently connected.")
        return
    try:
        ssid = wlan.config('ssid')
    except Exception:
        ssid = '?'
    wlan.disconnect()
    ok("Disconnected from '{}'.".format(ssid))

# ---------------------------------------------------------------------------
# Saved network management
# ---------------------------------------------------------------------------

def _read_networks():
    """Read saved networks from file. Returns list of (ssid, password) tuples."""
    try:
        with open(_NETWORKS_FILE, 'r') as f:
            nets = []
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split('\t', 1)
                ssid = parts[0].strip()
                pw   = parts[1].strip() if len(parts) > 1 else ''
                if ssid:
                    nets.append((ssid, pw))
            return nets
    except OSError:
        return []


def _write_networks(nets):
    """Write list of (ssid, password) tuples to file."""
    try:
        with open(_NETWORKS_FILE, 'w') as f:
            for ssid, pw in nets:
                f.write('{}\t{}\n'.format(ssid, pw))
    except OSError as e:
        error("Cannot write networks file: {}".format(e))


def list_saved():
    """Return list of (index, ssid) tuples for saved networks."""
    nets = _read_networks()
    return [(i + 1, ssid) for i, (ssid, _) in enumerate(nets)]


def add_saved(ssid, password):
    """Save a network. Replaces existing entry if SSID already saved."""
    nets = _read_networks()
    # Replace if already exists
    for i, (s, _) in enumerate(nets):
        if s.lower() == ssid.lower():
            nets[i] = (ssid, password)
            _write_networks(nets)
            ok("Network '{}' updated.".format(ssid))
            return True
    nets.append((ssid, password))
    _write_networks(nets)
    ok("Network '{}' saved.".format(ssid))
    return True


def forget_saved(ssid):
    """Remove a saved network by SSID. Returns True if found and removed."""
    nets = _read_networks()
    before = len(nets)
    nets = [(s, p) for s, p in nets if s.lower() != ssid.lower()]
    if len(nets) < before:
        _write_networks(nets)
        ok("Removed '{}'.".format(ssid))
        return True
    warn("'{}' not found in saved networks.".format(ssid))
    return False

# ---------------------------------------------------------------------------
# HTTP client
# ---------------------------------------------------------------------------

def _parse_url(url):
    """Parse a URL into (host, port, path, use_ssl)."""
    if url.startswith('https://'):
        rest    = url[8:]
        use_ssl = True
        port    = 443
    elif url.startswith('http://'):
        rest    = url[7:]
        use_ssl = False
        port    = 80
    else:
        raise ValueError("URL must begin with http:// or https://")

    slash = rest.find('/')
    if slash == -1:
        host_part = rest
        path      = '/'
    else:
        host_part = rest[:slash]
        path      = rest[slash:]

    if ':' in host_part:
        host, port_str = host_part.rsplit(':', 1)
        port = int(port_str)
    else:
        host = host_part

    return host, port, path, use_ssl


def _open_connection(host, port, use_ssl, timeout=15):
    """Open a TCP socket, optionally wrapped with TLS. Returns the socket."""
    import socket
    import gc
    gc.collect()
    ai   = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)
    addr = ai[0][-1]
    del ai
    gc.collect()
    s = socket.socket()
    try:
        s.settimeout(timeout)
    except Exception:
        pass
    s.connect(addr)
    del addr
    if use_ssl:
        # Two GC passes — second pass catches objects freed by first pass.
        gc.collect()
        gc.collect()
        # Heap-consolidation nudge: allocate a block large enough for the TLS
        # handshake buffer, then immediately free it.  This forces any pending
        # GC, and leaves a fresh contiguous region for ssl.wrap_socket() to use.
        try:
            _pre = bytearray(12000)
            del _pre
        except MemoryError:
            pass
        gc.collect()
        free = gc.mem_free()
        if free < 9500:
            raise MemoryError(
                "Not enough RAM for TLS ({} B free, need ~9500 B). "
                "If using Cloudflare, disable 'Always Use HTTPS' and use http:// URLs. "
                "Or reboot and try before loading other commands.".format(free)
            )
        try:
            import ssl
            s = ssl.wrap_socket(s, server_hostname=host)
        except ImportError:
            import ussl
            gc.collect()
            s = ussl.wrap_socket(s, server_hostname=host)
    return s


def _read_headers(s, chunk_size):
    """
    Read from socket until the end of HTTP headers (\\r\\n\\r\\n).
    Returns (raw_header_bytes, leftover_body_bytes).
    Uses bytearray.extend() to avoid creating a new bytes object on every chunk.
    """
    buf = bytearray()
    while True:
        chunk = s.recv(chunk_size)
        if not chunk:
            break
        buf.extend(chunk)
        if b'\r\n\r\n' in buf:
            break
        if len(buf) > 8192:
            break
    sep = buf.find(b'\r\n\r\n')
    if sep == -1:
        h = bytes(buf)
        del buf
        return h, b''
    h = bytes(buf[:sep])
    b = bytes(buf[sep + 4:])
    del buf
    return h, b


def _parse_status(raw_headers):
    """Extract HTTP status code from raw header bytes."""
    try:
        return int(raw_headers.decode('utf-8', 'ignore').split('\r\n')[0].split(' ')[1])
    except Exception:
        return 200


def _get_location(raw_headers):
    """Extract Location header value from raw header bytes, or None."""
    for line in raw_headers.decode('utf-8', 'ignore').split('\r\n')[1:]:
        if line.lower().startswith('location:'):
            return line.split(':', 1)[1].strip()
    return None


def _abs_url(location, orig_host, orig_ssl):
    """Resolve a Location header value into an absolute URL."""
    if location.startswith('http'):
        return location
    scheme = 'https://' if orig_ssl else 'http://'
    if location.startswith('/'):
        return scheme + orig_host + location
    return scheme + orig_host + '/' + location


def _content_length(raw_headers):
    """Return the Content-Length value from raw header bytes, or None."""
    for line in raw_headers.decode('utf-8', 'ignore').split('\r\n')[1:]:
        if line.lower().startswith('content-length:'):
            try:
                return int(line.split(':', 1)[1].strip())
            except ValueError:
                return None
    return None


def _draw_progress(done, total, last_pct):
    """Print an in-place download progress bar. Returns the new last_pct.

    Redraws only when the integer percent changes (total known) to avoid
    flooding the serial line; falls back to a byte counter when size unknown.
    """
    if total:
        pct = done * 100 // total
        if pct == last_pct:
            return last_pct
        filled = pct * 24 // 100
        sys.stdout.write('\r  [{}{}] {:>3}%  {}/{} B'.format(
            '#' * filled, '-' * (24 - filled), pct, done, total))
        return pct
    else:
        # Unknown size: update roughly every 4 KB
        if done - last_pct >= 4096:
            sys.stdout.write('\r  {} B'.format(done))
            return done
        return last_pct


def wget(url, dest=None, chunk_size=512, verbose=True):
    """
    Download a URL.
    - If dest is given: streams body directly to that file path, returns (status, bytes_written).
    - Otherwise: returns (status, body_bytes).
    Follows up to 5 redirects. Uses an iterative loop — no recursion — to keep
    heap flat during redirect chains.
    """
    import gc
    for _depth in range(6):
        if _depth > 0:
            gc.collect()
        host, port, path, use_ssl = _parse_url(url)
        if verbose:
            info("Connecting to {}:{}...".format(host, port))
        s = _open_connection(host, port, use_ssl)
        try:
            req = (
                'GET {} HTTP/1.0\r\n'
                'Host: {}\r\n'
                'User-Agent: RPCortex-Pulsar/0.9\r\n'
                'Connection: close\r\n\r\n'
            ).format(path, host)
            s.send(req.encode())
            raw_headers, body_buf = _read_headers(s, chunk_size)
            status = _parse_status(raw_headers)
            if verbose:
                info("HTTP {}".format(status))
            if status in (301, 302, 303, 307, 308):
                loc = _get_location(raw_headers)
                del raw_headers, body_buf
                gc.collect()
                if not loc:
                    raise OSError("Redirect with no Location header")
                url = _abs_url(loc, host, use_ssl)
                if verbose:
                    info("Redirect -> {}".format(url))
                continue   # next iteration opens a fresh connection
            # 2xx or other final response — read body
            if dest is not None:
                total = _content_length(raw_headers)
                written = 0
                last_pct = -1
                with open(dest, 'wb') as f:
                    if body_buf:
                        f.write(body_buf)
                        written += len(body_buf)
                    del body_buf
                    while True:
                        chunk = s.recv(chunk_size)
                        if not chunk:
                            break
                        f.write(chunk)
                        written += len(chunk)
                        if verbose:
                            last_pct = _draw_progress(written, total, last_pct)
                if verbose:
                    sys.stdout.write('\n')   # finish the progress line
                return status, written
            else:
                body = body_buf
                del body_buf
                while True:
                    chunk = s.recv(chunk_size)
                    if not chunk:
                        break
                    body += chunk
                return status, body
        finally:
            try:
                s.close()
            except Exception:
                pass
    raise OSError("Too many redirects")


def run_url(url, keep=False):
    """
    Download a Python file from a URL and execute it immediately.

    - keep=True  leaves the file at /Sandbox/runurl_tmp.py after execution
    - keep=False (default) deletes it after exec

    This runs in the current context (same as exec()).
    """
    import uos

    # Ensure sandbox directory exists
    try:
        uos.stat('/Sandbox')
    except OSError:
        try:
            uos.mkdir('/Sandbox')
        except OSError:
            pass

    tmp_path = '/Sandbox/runurl_tmp.py'
    info("Downloading: {}".format(url))

    try:
        status_code, size = wget(url, dest=tmp_path, verbose=True)
    except Exception as e:
        error("Download failed: {}".format(e))
        return

    if status_code != 200:
        error("Server returned HTTP {} — aborting execution.".format(status_code))
        try:
            uos.remove(tmp_path)
        except OSError:
            pass
        return

    ok("Downloaded {} bytes.  Executing...".format(size))

    try:
        with open(tmp_path, 'r') as f:
            code = f.read()
        exec(code)
    except Exception as e:
        error("Execution error: {}".format(e))
    finally:
        if not keep:
            try:
                uos.remove(tmp_path)
            except OSError:
                pass

# ---------------------------------------------------------------------------
# Ping  (TCP connect test — ICMP not available in standard MicroPython)
# ---------------------------------------------------------------------------

def ping(host, count=4, port=80):
    """
    TCP-based connectivity test. Connects to host:port and measures RTT.
    Real ICMP ping is unavailable on most MicroPython builds; this is the
    closest equivalent and works for diagnosing reachability.
    """
    import socket
    import utime

    info("PING {} (TCP/{})  {} packets".format(host, port, count))

    try:
        ai = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)
        ip = ai[0][-1][0]
        info("Resolved: {} -> {}".format(host, ip))
    except Exception as e:
        error("Cannot resolve '{}': {}".format(host, e))
        return

    addr = ai[0][-1]
    sent = received = total_ms = 0

    for i in range(count):
        s = socket.socket()
        try:
            s.settimeout(3)
        except Exception:
            pass
        t0 = utime.ticks_ms()
        try:
            s.connect(addr)
            ms = utime.ticks_diff(utime.ticks_ms(), t0)
            multi("  seq={} from {}  time={}ms".format(i + 1, ip, ms))
            received += 1
            total_ms += ms
        except OSError:
            multi("  seq={}  Request timeout".format(i + 1))
        finally:
            try:
                s.close()
            except Exception:
                pass
        sent += 1
        if i < count - 1:
            utime.sleep_ms(500)

    loss = (sent - received) * 100 // sent if sent else 100
    multi("")
    multi("  {} sent / {} received / {}% loss".format(sent, received, loss))
    if received:
        multi("  avg RTT: {}ms".format(total_ms // received))

# ---------------------------------------------------------------------------
# NSLookup
# ---------------------------------------------------------------------------

def nslookup(host):
    """DNS lookup — resolves a hostname and prints all returned addresses."""
    import socket

    info("Resolving '{}'...".format(host))
    try:
        results = socket.getaddrinfo(host, 80, 0, socket.SOCK_STREAM)
    except Exception as e:
        error("Lookup failed: {}".format(e))
        return

    seen = []
    for r in results:
        ip = r[-1][0]
        if ip not in seen:
            multi("  {} -> {}".format(host, ip))
            seen.append(ip)

    if not seen:
        warn("No addresses returned for '{}'.".format(host))

# ---------------------------------------------------------------------------
# Curl  — fetch URL body and return as string (for small text responses)
# ---------------------------------------------------------------------------

def curl(url, chunk_size=512, verbose=False, method='GET', data=None,
         headers=None, output=None, silent=False, head_only=False, timeout=15):
    """
    Fetch a URL and stream the response body to stdout.

    Default behaviour (GET, stream body to stdout, follow up to 5 redirects) is
    unchanged.  Optional flags extend it:
      method     HTTP verb (GET/POST/PUT/DELETE/...)
      data       request body (str/bytes); sets Content-Length
      headers    dict of extra request headers
      output     write the body to this file path instead of stdout
      silent     suppress status messages
      head_only  send HEAD and print only the response headers
      timeout    socket timeout in seconds

    Body is streamed chunk-by-chunk — never accumulated — so any response size
    works regardless of RAM.  Returns bytes written (or the status for HEAD).
    """
    import gc
    if head_only:
        method = 'HEAD'
    # Redirects are only auto-followed for a plain GET (a POST/HEAD redirect
    # would need method rewriting — out of scope for this first cut).
    follow = (method == 'GET' and data is None and not head_only)
    body_bytes = b''
    if data is not None:
        body_bytes = data.encode('utf-8') if isinstance(data, str) else data
    for _depth in range(6):
        if _depth > 0:
            gc.collect()
        host, port, path, use_ssl = _parse_url(url)
        if verbose and not silent:
            info("{} {}:{} -> {}".format(method, host, port, path))
        s = _open_connection(host, port, use_ssl, timeout=timeout)
        try:
            lines = [
                '{} {} HTTP/1.0'.format(method, path),
                'Host: {}'.format(host),
                'User-Agent: RPCortex-Pulsar/0.9',
            ]
            has_ct = False
            if headers:
                for k, v in headers.items():
                    lines.append('{}: {}'.format(k, v))
                    if k.lower() == 'content-type':
                        has_ct = True
            if body_bytes:
                if not has_ct:
                    lines.append('Content-Type: application/x-www-form-urlencoded')
                lines.append('Content-Length: {}'.format(len(body_bytes)))
            lines.append('Connection: close')
            s.send(('\r\n'.join(lines) + '\r\n\r\n').encode())
            if body_bytes:
                s.send(body_bytes)
            raw_headers, body_first = _read_headers(s, chunk_size)
            status = _parse_status(raw_headers)
            if verbose and not silent:
                info("HTTP {}".format(status))
            if follow and status in (301, 302, 303, 307, 308):
                loc = _get_location(raw_headers)
                del raw_headers, body_first
                gc.collect()
                if not loc:
                    raise OSError("Redirect with no Location header")
                url = _abs_url(loc, host, use_ssl)
                if verbose and not silent:
                    info("Redirect -> {}".format(url))
                continue
            if head_only:
                sys.stdout.write(raw_headers.decode('utf-8', 'ignore'))
                sys.stdout.write('\r\n')
                return status
            written = 0
            del raw_headers
            if output:
                with open(output, 'wb') as f:
                    if body_first:
                        f.write(body_first)
                        written += len(body_first)
                    del body_first
                    while True:
                        chunk = s.recv(chunk_size)
                        if not chunk:
                            break
                        f.write(chunk)
                        written += len(chunk)
                if not silent:
                    ok("Saved {} bytes to '{}'".format(written, output))
            else:
                if body_first:
                    sys.stdout.write(body_first.decode('utf-8', 'ignore'))
                    written += len(body_first)
                    del body_first
                while True:
                    chunk = s.recv(chunk_size)
                    if not chunk:
                        break
                    sys.stdout.write(chunk.decode('utf-8', 'ignore'))
                    written += len(chunk)
                sys.stdout.write('\r\n')
            return written
        finally:
            try:
                s.close()
            except Exception:
                pass
    raise OSError("Too many redirects")
