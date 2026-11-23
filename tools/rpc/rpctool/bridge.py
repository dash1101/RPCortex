"""The network bridge, the host's half (LINK.md, "The network bridge", protocol 3).

A board with no network of its own (a Pico, a Pico 2, an image built without
Wi-Fi) asks the computer to fetch a URL for it, in the middle of whatever
command it is running: `pkg install gps`, `update check`, `wget`. This answers
those requests. The computer makes the connection, checks HTTPS against its own
trust store, follows redirects, and streams the body back over the serial link,
one chunk per acknowledgement so the board's flash sets the pace.

What it will fetch is decided here, not by the device:

  * http and https only, on every hop of a redirect, and nothing else: the
    opener is built without the file, ftp and data handlers, so there is no way
    for a URL from the device to reach this computer's files;
  * an allowlist of hosts when one is given (--bridge-allow);
  * a size cap (--bridge-max).

Every request is logged, one line when it starts and one when it ends.
"""
import queue
import socket
import ssl
import threading
import time
import zlib

try:
    from urllib.error import HTTPError, URLError
    from urllib.parse import urlsplit
    from urllib.request import (HTTPDefaultErrorHandler, HTTPErrorProcessor, HTTPHandler,
                                HTTPRedirectHandler, HTTPSHandler, OpenerDirector, ProxyHandler,
                                Request)
except ImportError:  # pragma: no cover
    raise

from . import __version__
from .link import _b64
from .util import human

import base64

FRAME = b"\x1e"
TYPES = ("FETCH", "FHDR", "FBODY", "FGO", "FACK", "FSTOP")   # device to host
SHOWN = TYPES + ("BRIDGE",)                                   # kept off a terminal's screen
DEFAULT_MAX = 8 * 1024 * 1024
CHUNK = 768
WAIT_EVERY = 2.0           # FWAIT while the server is working (LINK.md)
ACK_WAIT = 20.0            # one FACK; the device is writing flash
COLLECT_WAIT = 5.0         # FETCH to FGO
SERVER_TIMEOUT = 30.0      # the server going quiet
REDIRECTS = 5
PASS_HEADERS = ("content-type", "etag", "last-modified", "location")
# What the device may not set: these are the host's to decide, or say something
# about the connection this side makes rather than about the request.
DROP_REQUEST = ("host", "user-agent", "accept-encoding", "connection", "content-length",
                "transfer-encoding", "te", "upgrade", "proxy-authorization", "proxy-connection",
                "keep-alive", "expect", "cookie")


class Refused(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code
        self.msg = msg


def parse_size(text):
    """'8M', '512K', '1048576' -> bytes."""
    t = str(text).strip().upper().rstrip("B")
    mult = 1
    if t.endswith("K"):
        mult, t = 1024, t[:-1]
    elif t.endswith("M"):
        mult, t = 1024 * 1024, t[:-1]
    elif t.endswith("G"):
        mult, t = 1024 * 1024 * 1024, t[:-1]
    return int(float(t) * mult)


class Policy(object):
    """What this computer agrees to fetch for a device."""

    def __init__(self, allow=None, max_bytes=DEFAULT_MAX):
        self.allow = [a.strip().lower() for a in (allow or []) if a and a.strip()]
        self.max_bytes = max_bytes

    def check(self, url):
        """Raise Refused unless `url` may be fetched."""
        try:
            u = urlsplit(url)
        except ValueError:
            raise Refused("refused", "not a URL")
        if u.scheme not in ("http", "https"):
            raise Refused("refused", "only http and https are bridged, not {}".format(
                u.scheme or "a bare path"))
        host = (u.hostname or "").lower()
        if not host:
            raise Refused("refused", "no host in the URL")
        listed = any(host == a or (a.startswith(".") and host.endswith(a)) or
                     (a.startswith("*.") and host.endswith(a[1:])) for a in self.allow)
        if self.allow and not listed and "*" not in self.allow:
            raise Refused("refused", "{} is not on this computer's bridge allowlist".format(host))
        # This computer itself (localhost, 127.x, ::1) and link-local addresses
        # (169.254.x, where cloud machines keep their credentials) are services
        # meant for this computer, not for a device on its USB port. Named in the
        # allowlist, they are allowed.
        if not listed and _is_local(host):
            raise Refused("refused", "{} is this computer itself or link-local; name it with "
                                     "--bridge-allow to fetch from it".format(host))


def _is_local(host):
    """Whether `host` is, or resolves to, a loopback or link-local address."""
    import ipaddress
    h = host.strip("[]")
    try:
        addrs = [ipaddress.ip_address(h)]
    except ValueError:
        if h == "localhost" or h.endswith(".localhost"):
            return True
        try:
            addrs = [ipaddress.ip_address(i[4][0].split("%")[0])
                     for i in socket.getaddrinfo(h, None)]
        except (socket.gaierror, OSError, ValueError):
            return False            # unresolvable: the fetch will fail on its own
    return any(a.is_loopback or a.is_link_local or a.is_unspecified for a in addrs)


class _Redirects(HTTPRedirectHandler):
    """Follow a redirect only where the first URL could have gone."""

    max_redirections = REDIRECTS

    def __init__(self, policy):
        self.policy = policy

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        self.policy.check(newurl)          # Refused propagates out of open()
        return HTTPRedirectHandler.redirect_request(self, req, fp, code, msg, headers, newurl)


def _opener(policy):
    """http and https and nothing else: no FileHandler, FTPHandler or DataHandler."""
    o = OpenerDirector()
    for h in (ProxyHandler(), HTTPHandler(), HTTPSHandler(context=ssl.create_default_context()),
              _Redirects(policy), HTTPDefaultErrorHandler(), HTTPErrorProcessor()):
        o.add_handler(h)
    return o


def _frame_id(frame):
    try:
        return int(frame.rest.split(" ", 1)[0])
    except ValueError:
        return None


def _after_id(frame):
    parts = frame.rest.split(" ", 1)
    return parts[1] if len(parts) > 1 else ""


class Bridge(object):
    """Answers the device's FETCH frames. One fetch at a time.

    `log(text)` gets a line per request; `progress(label)` may return a callable
    (done, total) for a progress bar, with a done() method, or None."""

    def __init__(self, policy=None, log=None, progress=None, opener=None):
        self.policy = policy or Policy()
        self.log = log or (lambda text: None)
        self.progress = progress
        self._opener = opener
        self.busy = False
        self.served = []          # (method, url, status or error code, bytes), for tests and logs

    # --- the wire ---------------------------------------------------------

    @staticmethod
    def _line(write, text):
        write(FRAME + text.encode("utf-8", "surrogateescape") + b"\n")

    def serve(self, first, next_frame, write, interrupt=None):
        """Answer the FETCH frame `first`. `next_frame(timeout)` gives the
        device's next frame or None; `write(bytes)` sends to it. `interrupt()`,
        when given, is asked as the fetch goes: true sends the device a Ctrl+C,
        which stops the fetch from its side (FSTOP) as one typed there would.
        Returns the frames read on the way that were not this fetch's, in
        order, for the caller to handle (a command's OK after the device gave
        up, say)."""
        self.busy = True
        if interrupt is not None:
            plain = next_frame

            def next_frame(timeout):
                if interrupt():
                    write(b"\x03")
                return plain(timeout)
        try:
            return self._serve(first, next_frame, write)
        finally:
            self.busy = False

    def _serve(self, first, next_frame, write):
        leftover = []
        rid = _frame_id(first)
        rest = _after_id(first)
        method, _, url = rest.partition(" ")
        if rid is None or not method or not url:
            return leftover
        headers, body, chunk = [], bytearray(), CHUNK
        # Up to FGO: the request's headers and body.
        while True:
            f = next_frame(COLLECT_WAIT)
            if f is None:
                return leftover                      # the device gave up before asking
            if f.type in ("FHDR", "FBODY", "FGO") and _frame_id(f) == rid:
                if f.type == "FHDR":
                    try:
                        headers.append(_b64(_after_id(f)).decode("latin-1"))
                    except Exception:
                        pass
                elif f.type == "FBODY":
                    body.extend(_b64(_after_id(f)))
                else:
                    try:
                        chunk = max(1, min(int(f.kv().get("chunk", CHUNK)), CHUNK))
                    except ValueError:
                        chunk = CHUNK
                    break
            elif f.type == "FETCH":
                leftover.append(f)                   # a new one: the old was abandoned
                return leftover
            else:
                leftover.append(f)
        return leftover + self._answer(rid, method, url, headers, bytes(body), chunk,
                                       next_frame, write)

    def _answer(self, rid, method, url, headers, body, chunk, next_frame, write):
        leftover = []
        say = lambda text: self._line(write, text)
        say("FWAIT {}".format(rid))
        t0 = time.monotonic()
        try:
            self.policy.check(url)
        except Refused as e:
            self.log("bridge: {} {} refused: {}".format(method, url, e.msg))
            say("FDONE {} err {} {}".format(rid, e.code, e.msg))
            self.served.append((method, url, e.code, 0))
            return leftover
        self.log("bridge: {} {}".format(method, url))

        q = queue.Queue()
        stop = threading.Event()
        worker = threading.Thread(target=self._fetch, args=(method, url, headers, body, q, stop))
        worker.daemon = True
        worker.start()

        def finish(code, msg, status=None, got=0):
            stop.set()
            if code:
                say("FDONE {} err {} {}".format(rid, code, msg))
                self.log("bridge: {} {} failed: {}".format(method, url, msg))
                self.served.append((method, url, code, got))
            else:
                say("FDONE {} ok".format(rid))
                secs = time.monotonic() - t0
                self.log("bridge: {} {} -> {}, {} in {:.1f} s".format(
                    method, url, status, human(got), secs))
                self.served.append((method, url, status, got))

        def device_frame(timeout):
            """('stop' | 'ack' <n> | 'other' <frame> | None)."""
            f = next_frame(timeout)
            if f is None:
                return None
            if f.type == "FSTOP" and _frame_id(f) == rid:
                return ("stop", None)
            if f.type == "FACK" and _frame_id(f) == rid:
                try:
                    return ("ack", int(_after_id(f).split(" ")[0]))
                except ValueError:
                    return ("ack", -1)
            leftover.append(f)
            return ("other", f)

        # The response head, with FWAIT every couple of seconds while the server works.
        last_wait = time.monotonic()
        while True:
            try:
                item = q.get(timeout=0.05)
                break
            except queue.Empty:
                pass
            d = device_frame(0.02)
            if d and d[0] == "stop":
                finish("cancelled", "stopped by the device")
                return leftover
            if d and d[0] == "other" and d[1].final:
                stop.set()
                return leftover                      # the command ended without waiting
            if time.monotonic() - last_wait >= WAIT_EVERY:
                if not worker.is_alive() and q.empty():
                    finish("net", "the fetch on this computer stopped without an answer")
                    return leftover
                say("FWAIT {}".format(rid))
                last_wait = time.monotonic()
        if item[0] == "err":
            finish(item[1], item[2])
            return leftover
        _, status, passed, length = item
        say("FHEAD {} {}{}".format(rid, status, " len={}".format(length) if length is not None else ""))
        for h in passed:
            say("FHDR {} {}".format(rid, base64.b64encode(h.encode("latin-1", "replace")).decode("ascii")))

        bar = self.progress(url.rsplit("/", 1)[-1] or url) if self.progress else None
        pending = bytearray()
        ended = False
        got = 0
        try:
            while True:
                # Gather a chunk's worth, or whatever there is at the end.
                while not ended and len(pending) < chunk:
                    try:
                        item = q.get(timeout=WAIT_EVERY)
                    except queue.Empty:
                        if not worker.is_alive() and q.empty():
                            finish("net", "the fetch on this computer stopped part way", got=got)
                            return leftover
                        say("FWAIT {}".format(rid))      # the server is slow; still here
                        d = device_frame(0.02)
                        if d and d[0] == "stop":
                            finish("cancelled", "stopped by the device", got=got)
                            return leftover
                        continue
                    if item[0] == "data":
                        pending.extend(item[1])
                    elif item[0] == "end":
                        ended = True
                    else:
                        finish(item[1], item[2], got=got)
                        return leftover
                if not pending and ended:
                    finish(None, None, status, got)
                    return leftover
                piece = bytes(pending[:chunk])
                del pending[:chunk]
                say("FDATA {} {}".format(rid, base64.b64encode(piece).decode("ascii")))
                got += len(piece)
                if bar:
                    bar(got, length or 0)
                # One chunk in flight: nothing more until the device has dealt with it.
                deadline = time.monotonic() + ACK_WAIT
                while True:
                    d = device_frame(max(0.0, deadline - time.monotonic()))
                    if d is None:
                        stop.set()
                        self.log("bridge: {} {}: the device stopped acknowledging".format(method, url))
                        return leftover
                    if d[0] == "stop":
                        finish("cancelled", "stopped by the device", got=got)
                        return leftover
                    if d[0] == "ack":
                        break
                    if d[1].final:
                        stop.set()
                        return leftover
        finally:
            stop.set()
            if bar and hasattr(bar, "done"):
                bar.done()

    # --- the fetch, on its own thread ---------------------------------------

    def _fetch(self, method, url, headers, body, q, stop):
        try:
            req = Request(url, data=body if body or method in ("POST", "PUT", "PATCH") else None,
                          method=method)
            for h in headers:
                name, _, value = h.partition(":")
                if not name.strip() or "\r" in h or "\n" in h:
                    continue
                if name.strip().lower() in DROP_REQUEST:
                    continue
                req.add_header(name.strip(), value.strip())
            req.add_header("User-Agent", "rpctool/{} (RPCortex network bridge)".format(__version__))
            req.add_header("Accept-Encoding", "identity")
            opener = self._opener or _opener(self.policy)
            try:
                resp = opener.open(req, timeout=SERVER_TIMEOUT)
            except HTTPError as e:
                resp = e                         # a 404 is an answer, not a failure
            status = resp.getcode() if hasattr(resp, "getcode") else resp.code
            hdrs = resp.headers
            enc = (hdrs.get("Content-Encoding") or "identity").strip().lower()
            if enc not in ("identity", "", "gzip", "x-gzip", "deflate"):
                q.put(("err", "net", "the server sent {} encoding, which cannot be undone here".format(enc)))
                return
            length = None
            if enc in ("identity", "") and hdrs.get("Content-Length", "").strip().isdigit():
                length = int(hdrs.get("Content-Length").strip())
                if length > self.policy.max_bytes:
                    q.put(("err", "toobig", "{} is over this computer's {} bridge limit".format(
                        human(length), human(self.policy.max_bytes))))
                    return
            passed = []
            for name in PASS_HEADERS:
                v = hdrs.get(name)
                if v and "\r" not in v and "\n" not in v:
                    line = "{}: {}".format("-".join(p.capitalize() for p in name.split("-")), v)
                    if name == "etag":
                        line = "ETag: " + v
                    if len(line) < 200:
                        passed.append(line)
            q.put(("head", status, passed, None if method == "HEAD" else length))
            if method == "HEAD":
                q.put(("end",))
                return
            unz = zlib.decompressobj(32 + zlib.MAX_WBITS) if enc != "identity" and enc else None
            total = 0
            while not stop.is_set():
                data = resp.read(16384)
                if not data:
                    break
                if unz:
                    data = unz.decompress(data)
                total += len(data)
                if total > self.policy.max_bytes:
                    q.put(("err", "toobig", "over this computer's {} bridge limit".format(
                        human(self.policy.max_bytes))))
                    return
                if data:
                    q.put(("data", data))
            if unz and not stop.is_set():
                tail = unz.flush()
                if tail:
                    q.put(("data", tail))
            q.put(("end",))
        except Refused as e:
            q.put(("err", e.code, e.msg))
        except HTTPError as e:
            q.put(("err", "net", "HTTP {}".format(e.code)))
        except URLError as e:
            r = e.reason
            if isinstance(r, Refused):
                q.put(("err", r.code, r.msg))
            else:
                q.put(("err", "net", str(r)))
        except (socket.timeout, TimeoutError):
            q.put(("err", "timeout", "no answer from the server in {:.0f} s".format(SERVER_TIMEOUT)))
        except (ssl.SSLError, ssl.CertificateError) as e:
            q.put(("err", "net", "the certificate did not verify: {}".format(e)))
        except (OSError, ValueError, zlib.error) as e:
            q.put(("err", "net", str(e) or e.__class__.__name__))
        except Exception as e:  # whatever it was, the device must hear the fetch ended
            q.put(("err", "net", "{}: {}".format(e.__class__.__name__, e)))
