"""The link protocol client (LINK.md, protocols 1 to 3).

One request at a time, read to its final OK or ERR before the next is sent.
Anything on the wire that is not a frame is a background line from the device
and goes to `on_noise`, never to a parser.

Protocol 3: a command can ask the host to fetch a URL in the middle of running
(the network bridge). With `bridge` set (rpctool.bridge.Bridge), the FETCH
frames that arrive while a request waits are answered on the spot.
"""
import base64
import binascii
import hashlib
import time

try:
    from urllib.parse import unquote
except ImportError:  # pragma: no cover
    from urllib import unquote

from .errors import (DeviceTimeout, DigestMismatch, LinkError, ProtocolError,
                     RpcError)
from .transport import strip_ansi

FRAME = b"\x1e"
LINE_MAX = 1100          # a request line, newline included (LINK.md)
DEFAULT_CHUNK = 768
FINALS = ("OK", "ERR")


class Timing(object):
    """Every wait, in seconds, in one place so tests can shrink them."""
    settle = 0.25         # after a Ctrl+C, before reading what came back
    enter = 4.0           # from `link` to the HELLO frame
    request = 10.0        # an ordinary request to its final frame
    transfer = 15.0       # one ACK, D or final frame during PUT and GET
    abort = 15.0          # a cancelled request to its final frame
    bye = 2.0
    type_delay = 0.004    # between characters typed at the prompt (legacy, login)
    legacy_put_ack = 20.0


class Frame(object):
    """One device-to-host frame: the bytes after 0x1E, without the newline."""

    __slots__ = ("type", "rest", "raw")

    def __init__(self, raw):
        self.raw = raw
        text = raw.decode("utf-8", "surrogateescape")
        head, _, rest = text.partition(" ")
        self.type = head
        self.rest = rest

    def kv(self):
        out = {}
        for word in self.rest.split(" "):
            k, eq, v = word.partition("=")
            if eq:
                out[k] = v
        return out

    @property
    def final(self):
        return self.type in FINALS

    def error(self):
        code, _, msg = self.rest.partition(" ")
        return LinkError(code or "?", msg)

    def __repr__(self):
        return "Frame({!r})".format(self.raw[:80])


class Entry(object):
    """One LS entry."""

    __slots__ = ("type", "size", "name")

    def __init__(self, type_, size, name):
        self.type = type_
        self.size = size
        self.name = name

    @property
    def is_dir(self):
        return self.type == "d"

    def __repr__(self):
        return "Entry({!r}, {}, {!r})".format(self.type, self.size, self.name)


class ExecResult(object):
    __slots__ = ("rc", "output", "truncated", "timed_out")

    def __init__(self, rc, output, truncated=False, timed_out=False):
        self.rc = rc
        self.output = output
        self.truncated = truncated
        self.timed_out = timed_out

    @property
    def text(self):
        return self.output.decode("utf-8", "replace")


def _noise_text(line):
    text = strip_ansi(line).decode("utf-8", "replace")
    # The line editor repaints with a carriage return; what is left after the
    # last one is what a terminal would show.
    if "\r" in text.rstrip("\r"):
        text = text.rstrip("\r").split("\r")[-1]
    return text.rstrip("\r")


class FrameReader(object):
    """Splits the byte stream into frames and everything else.

    A frame is found wherever 0x1E appears, not only at the start of a line: a
    background task that printed without a newline would otherwise hide the
    frame that follows it. A trailing carriage return is dropped, since the
    SDK's console may end lines with CRLF.
    """

    def __init__(self, transport, on_noise=None):
        self.t = transport
        self.on_noise = on_noise
        self.buf = bytearray()
        self.pending = []         # frames read ahead and handed back (unread)

    def feed(self, data):
        self.buf.extend(data)

    def unread(self, frames):
        """Put frames back, to be returned before anything new."""
        self.pending[:0] = list(frames)

    def _take_line(self):
        i = self.buf.find(b"\n")
        if i < 0:
            return None
        line = bytes(self.buf[:i])
        del self.buf[:i + 1]
        return line

    def _noise(self, line):
        if self.on_noise and line.strip(b"\r"):
            self.on_noise(_noise_text(line))

    def next_frame(self, timeout):
        """The next frame, or None after `timeout` seconds (None: wait for ever)."""
        if self.pending:
            return self.pending.pop(0)
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            line = self._take_line()
            while line is not None:
                at = line.find(FRAME)
                if at < 0:
                    self._noise(line)
                else:
                    if at:
                        self._noise(line[:at])
                    body = line[at + 1:]
                    if body.endswith(b"\r"):
                        body = body[:-1]
                    return Frame(body)
                line = self._take_line()
            if deadline is None:
                wait = 0.5
            else:
                wait = deadline - time.monotonic()
                if wait <= 0:
                    return None
                wait = min(wait, 0.5)
            data = self.t.read(wait)
            if data:
                self.buf.extend(data)


def parse_hello(frame):
    info = frame.kv()
    if "cwd" in info:
        info["cwd"] = unquote(info["cwd"])
    return info


class LinkClient(object):
    """Requests over a transport that is already in link mode."""

    def __init__(self, transport, reader=None, on_noise=None, timing=None):
        self.t = transport
        self.r = reader or FrameReader(transport, on_noise)
        self.timing = timing or Timing()
        self.hello = {}
        self.busy = None          # the request in flight: exec, put, get, req
        self.dropped = False      # link mode ended under us (a stray Ctrl+C)
        self.bridge = None        # answers FETCH frames (protocol 3), when set
        self.bridged = False      # this client said BRIDGE on

    # --- basics ---------------------------------------------------------

    @property
    def chunk(self):
        try:
            return max(1, min(int(self.hello.get("chunk", DEFAULT_CHUNK)), DEFAULT_CHUNK))
        except ValueError:
            return DEFAULT_CHUNK

    @property
    def exec_max(self):
        try:
            return int(self.hello.get("exec_max", 0))
        except ValueError:
            return 0

    def set_hello(self, frame):
        self.hello = parse_hello(frame)
        return self.hello

    @staticmethod
    def encode(line):
        """The request as bytes, refused here if the device could not take it."""
        data = line.encode("utf-8", "surrogateescape")
        if b"\n" in data or b"\r" in data:
            raise RpcError("a request cannot contain a line break")
        if len(data) + 1 > LINE_MAX:
            raise RpcError("the request is {} bytes; the device takes at most {}"
                           .format(len(data) + 1, LINE_MAX))
        return data + b"\n"

    def send(self, line):
        self.t.write(self.encode(line))

    def next(self, timeout, interrupt=None):
        """The next frame, or None after `timeout`, answering any FETCH on the way
        (the network bridge) when this client has one to answer with.
        `interrupt()` true during a fetch sends the device a Ctrl+C."""
        while True:
            f = self.r.next_frame(timeout)
            if f is None or f.type != "FETCH" or self.bridge is None:
                return f
            left = self.bridge.serve(f, self.r.next_frame, self.t.write, interrupt)
            self.r.unread(left)

    def frame(self, timeout):
        f = self.next(timeout)
        if f is None:
            raise DeviceTimeout("no answer from the device in {:.0f} s".format(timeout))
        return f

    def final(self, timeout, keep=None):
        """Read frames to the final one; others are added to `keep`."""
        while True:
            f = self.frame(timeout)
            if f.final:
                return f
            if f.type == "BYE":
                # The device left link mode (Ctrl+C on its console, or the
                # port was closed and reopened) instead of answering.
                self.dropped = True
                raise RpcError("the device left link mode before answering; reconnect")
            if keep is not None:
                keep.append(f)

    def _begin(self, kind):
        if self.dropped:
            raise RpcError("link mode has ended; reconnect")
        self.busy = kind

    def _guard(self, fn):
        """Run fn; if it is interrupted with a request still in flight, put the
        device back to waiting for a request before passing the error on."""
        try:
            return fn()
        except BaseException:
            if self.busy:
                kind, self.busy = self.busy, None
                try:
                    self.abort(kind)
                except BaseException:
                    pass
            raise

    def request(self, line, timeout=None):
        """Send one request; return (final frame, intermediate frames).
        ERR is raised as LinkError."""
        data = self.encode(line)

        def run():
            self._begin("req")
            self.t.write(data)
            keep = []
            f = self.final(timeout or self.timing.request, keep)
            self.busy = None
            if f.type == "ERR":
                raise f.error()
            return f, keep
        return self._guard(run)

    # --- recovery -------------------------------------------------------

    def abort(self, kind):
        """Stop whatever `kind` of request is in flight and resynchronise."""
        if kind in ("put", "get"):
            self.t.write(b"CANCEL\n")
        elif kind == "exec":
            self.t.write(b"\x03")
        try:
            self.final(self.timing.abort)
        except DeviceTimeout:
            pass
        self.resync()

    def resync(self, timeout=None):
        """Make sure the next frame read answers the next request sent.

        A HELLO request is answered by a HELLO frame and then OK, so anything
        stale (a final frame for a request that was already over, say) is read
        and dropped before it. No HELLO means link mode has ended.
        """
        timeout = timeout or self.timing.bye * 2
        self.t.write(b"HELLO\n")
        end = time.monotonic() + timeout
        seen = False
        while True:
            left = end - time.monotonic()
            f = self.r.next_frame(max(0.0, left)) if left > 0 else None
            if f is None:
                self.dropped = True
                return False
            if f.type == "HELLO":
                self.set_hello(f)
                seen = True
            elif seen and f.final:
                return True

    # --- requests -------------------------------------------------------

    def ping(self):
        self.request("PING")

    @property
    def proto(self):
        try:
            return int(self.hello.get("proto", "1"))
        except ValueError:
            return 1

    def auth(self, password):
        """AUTH (protocol 2): prove the signed-in account's password, so the
        commands that need sudo can run for the next few minutes. Returns the
        seconds the device says it counts for. A wrong one is LinkError 'auth'."""
        pw = password.encode("utf-8") if isinstance(password, str) else bytes(password)
        f, _ = self.request("AUTH " + base64.b64encode(pw).decode("ascii"))
        try:
            return int(f.kv().get("grace", "0"))
        except ValueError:
            return 0

    @property
    def can_bridge(self):
        """The firmware has the bridge and no network of its own (HELLO bridge=1)."""
        return self.proto >= 3 and self.hello.get("bridge") == "1"

    def bridge_on(self):
        """BRIDGE on (protocol 3): this host will answer the device's fetches.
        True when the device took it."""
        if not self.can_bridge:
            return False
        f, _ = self.request("BRIDGE on")
        self.bridged = f.kv().get("bridge") == "on"
        return self.bridged

    def bridge_off(self):
        """BRIDGE off, before leaving, so nothing is sent to a host that has gone."""
        if not self.bridged or self.dropped:
            return
        try:
            self.request("BRIDGE off")
        except RpcError:
            pass
        self.bridged = False

    def hello_request(self):
        f, keep = self.request("HELLO")
        for k in keep:
            if k.type == "HELLO":
                return self.set_hello(k)
        raise ProtocolError("HELLO was answered without a HELLO frame")

    def ls(self, path="."):
        f, keep = self.request("LS " + (path or "."))
        out = []
        for k in keep:
            if k.type != "E":
                continue
            parts = k.rest.split(" ", 2)
            if len(parts) != 3:
                raise ProtocolError("malformed entry: {!r}".format(k.raw))
            out.append(Entry(parts[0], int(parts[1]), parts[2]))
        return out

    def stat(self, path):
        """(type, size) or None when the path does not exist."""
        try:
            f, _ = self.request("STAT " + path)
        except LinkError as e:
            if e.code == "nofile":
                return None
            raise
        kv = f.kv()
        return kv.get("type", "?"), int(kv.get("size", "0"))

    def exec(self, line, timeout=None):
        """Run a shell line. On `timeout` seconds a Ctrl+C is sent and the result
        comes back with timed_out set."""
        data = self.encode("EXEC " + line)

        def run():
            self._begin("exec")
            self.t.write(data)
            out = bytearray()
            stopped = False
            deadline = None if not timeout else time.monotonic() + timeout
            state = {"stopped": False}

            def due():
                # --timeout running out in the middle of a fetch: a Ctrl+C, once.
                if deadline is None or state["stopped"] or time.monotonic() < deadline:
                    return False
                state["stopped"] = True
                return True
            while True:
                wait = None if deadline is None else max(0.0, deadline - time.monotonic())
                f = self.next(wait, due)
                if state["stopped"] and not stopped:
                    stopped = True
                    deadline = time.monotonic() + self.timing.abort
                if f is None:
                    if not stopped:
                        self.t.write(b"\x03")
                        stopped = True
                        deadline = time.monotonic() + self.timing.abort
                        continue
                    raise DeviceTimeout("the command did not stop after Ctrl+C")
                if f.type == "OUT":
                    out.extend(_b64(f.rest))
                elif f.type == "OK":
                    self.busy = None
                    kv = f.kv()
                    return ExecResult(int(kv.get("rc", "1")), bytes(out),
                                      kv.get("truncated") == "1", stopped)
                elif f.type == "ERR":
                    self.busy = None
                    raise f.error()
        return self._guard(run)

    def put(self, data, path, progress=None):
        """Write `data` to `path`. Returns the digest the device confirmed."""
        data = bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        req = self.encode("PUT {} {} {}".format(len(data), digest, path))

        def run():
            self._begin("put")
            self.t.write(req)
            f = self.frame(self.timing.transfer)
            while not (f.type == "READY" or f.final):
                f = self.frame(self.timing.transfer)
            if f.type == "ERR":
                self.busy = None
                raise f.error()
            if f.type == "OK":
                raise ProtocolError("PUT was answered OK before READY")
            try:
                chunk = max(1, min(int(f.kv().get("chunk", self.chunk)), self.chunk))
            except ValueError:
                chunk = self.chunk
            sent = 0
            if progress:
                progress(0, len(data))
            while sent < len(data):
                piece = data[sent:sent + chunk]
                self.t.write(b"D " + base64.b64encode(piece) + b"\n")
                sent += len(piece)
                while True:
                    f = self.frame(self.timing.transfer)
                    if f.type == "ACK":
                        break
                    if f.type == "ERR":
                        self.busy = None
                        raise f.error()
                    if f.type == "OK":
                        raise ProtocolError("PUT ended early, after {} bytes".format(sent))
                try:
                    acked = int(f.rest.split(" ")[0])
                except ValueError:
                    raise ProtocolError("malformed ACK: {!r}".format(f.raw))
                if acked != sent:
                    raise ProtocolError("the device acknowledged {} bytes, {} were sent"
                                        .format(acked, sent))
                if progress:
                    progress(sent, len(data))
            f = self.final(self.timing.transfer)
            self.busy = None
            if f.type == "ERR":
                raise f.error()
            got = f.kv().get("sha", "").lower()
            if got != digest:
                raise DigestMismatch("the device reports sha256 {}, expected {}"
                                     .format(got or "(none)", digest))
            return digest
        return self._guard(run)

    def get(self, path, progress=None):
        """Read a file. Returns its bytes, checked against the device's digest."""
        req = self.encode("GET " + path)

        def run():
            self._begin("get")
            self.t.write(req)
            f = self.frame(self.timing.transfer)
            while not (f.type == "SIZE" or f.final):
                f = self.frame(self.timing.transfer)
            if f.type == "ERR":
                self.busy = None
                raise f.error()
            if f.type == "OK":
                raise ProtocolError("GET was answered OK before SIZE")
            try:
                size = int(f.rest.split(" ")[0])
            except ValueError:
                raise ProtocolError("malformed SIZE: {!r}".format(f.raw))
            data = bytearray()
            if progress:
                progress(0, size)
            while True:
                f = self.frame(self.timing.transfer)
                if f.type == "D":
                    data.extend(_b64(f.rest))
                    if progress:
                        progress(len(data), size)
                    self.t.write(b"A\n")
                elif f.type == "ERR":
                    self.busy = None
                    raise f.error()
                elif f.type == "OK":
                    self.busy = None
                    break
            want = f.kv().get("sha", "").lower()
            have = hashlib.sha256(bytes(data)).hexdigest()
            if len(data) != size:
                raise DigestMismatch("{}: {} bytes arrived, the device said {}"
                                     .format(path, len(data), size))
            if want != have:
                raise DigestMismatch("{}: the device sent sha256 {}, what arrived is {}"
                                     .format(path, want or "(none)", have))
            return bytes(data)
        return self._guard(run)

    def bye(self):
        """Leave link mode. True when the device said OK."""
        if self.dropped:
            return False
        try:
            self.t.write(b"BYE\n")
            end = time.monotonic() + self.timing.bye
            while True:
                left = end - time.monotonic()
                if left <= 0:
                    return False
                f = self.r.next_frame(left)
                if f is None:
                    return False
                if f.final:
                    self.dropped = True
                    return f.type == "OK"
        except RpcError:
            return False


def _b64(text):
    try:
        return base64.b64decode(text.strip().encode("ascii"), validate=True)
    except (binascii.Error, ValueError, UnicodeEncodeError):
        raise ProtocolError("bad base64 from the device")
