"""Talking to firmware older than link mode, through the prompt a person uses.

Two things work there, both exactly as the tools did before LINK.md existed:

  put   `put <name> <len> <sha256>` and the raw bytes, waiting for one 0x06
        after every 256 (tools/putfile.py, the same three rules: DTR, the ACK,
        the digest);
  exec  the command typed at the prompt with a marker after it, reading to
        the marker.

Nothing comes back the other way: no GET, no LS, no STAT.
"""
import binascii
import hashlib
import os
import re
import time

from .errors import DeviceTimeout, DigestMismatch, NotSupported, RpcError
from .transport import strip_ansi
from .util import quote

PUT_CHUNK = 256      # PUT_CHUNK in os/shell/shell.cpp
PUT_ACK = 0x06
# The prompt's buffer is `char line[128]` in os/shell/shell.cpp, and the line
# editor silently stops taking characters when it is full.
PROMPT_MAX = 127


class LegacyShell(object):
    def __init__(self, transport, timing, on_noise=None):
        self.t = transport
        self.timing = timing
        self.on_noise = on_noise

    # --- typing ---------------------------------------------------------

    def type_line(self, text):
        """Type a line one character at a time, the way a person would.

        A whole line arriving in one burst was seen to come in reordered once
        (tools/soak.py), and the line editor is built for keystrokes."""
        data = text.encode("utf-8")
        if any(b < 32 or b > 126 for b in bytearray(data)):
            # The line editor drops everything else, so a name with an accent
            # in it would silently become a different name.
            raise NotSupported("this firmware's prompt takes printable ASCII only; "
                               "{!r} cannot be typed there".format(text))
        if len(data) > PROMPT_MAX:
            # Typed anyway, the end of it (the marker, or the digest) would be
            # dropped and the answer would never come.
            raise NotSupported("this firmware's prompt takes at most {} characters, and this "
                               "needs {}; shorten it, or update the firmware"
                               .format(PROMPT_MAX, len(data)))
        for i in range(len(data)):
            self.t.write(data[i:i + 1])
            if self.timing.type_delay:
                time.sleep(self.timing.type_delay)
        self.t.write(b"\r")

    def clear_line(self):
        """Ctrl+C abandons anything half-typed and gives a fresh prompt."""
        self.t.write(b"\x03")
        self._collect(self.timing.settle)

    def _collect(self, seconds):
        buf = bytearray()
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return bytes(buf)
            buf.extend(self.t.read(min(left, 0.1)))

    # --- exec -----------------------------------------------------------

    def run(self, line, timeout=None):
        """Run a line at the prompt. Returns (rc, output bytes, timed_out).

        The status comes from the shell's own chaining: `&& echo ..0 || echo ..1`
        prints 0 when the line succeeded, by the same rule EXEC uses (a command
        that printed an error failed). The marker is typed with quotes between
        its halves and printed without them, so what the line editor echoes
        (and repaints) can never be mistaken for the answer, and the answer is
        found even after output that did not end its last line.
        """
        if "\n" in line or "\r" in line:
            raise RpcError("a command line cannot contain a line break")
        if not line.strip():
            raise RpcError("nothing to run")
        tok = binascii.hexlify(os.urandom(4)).decode()
        typed = '{} && echo "RPCM" "{}0" || echo "RPCM" "{}1"'.format(line, tok, tok)
        done = re.compile(r"RPCM " + tok + r"([01])")
        self.clear_line()
        self.type_line(typed)

        buf = bytearray()
        timed_out = False
        deadline = None if not timeout else time.monotonic() + timeout
        try:
            while True:
                if deadline is not None and time.monotonic() >= deadline:
                    if timed_out:
                        raise DeviceTimeout("the command did not stop after Ctrl+C")
                    self.t.write(b"\x03")
                    timed_out = True
                    deadline = time.monotonic() + self.timing.abort
                buf.extend(self.t.read(0.2))
                text = strip_ansi(bytes(buf)).decode("utf-8", "replace")
                m = done.search(text)
                if m:
                    return int(m.group(1)), _output(text[:m.start()]).encode("utf-8"), timed_out
        except KeyboardInterrupt:
            self.t.write(b"\x03")       # stop the command, not just the waiting
            raise

    # --- put ------------------------------------------------------------

    def put(self, data, path, progress=None):
        data = bytes(data)
        if not data:
            raise NotSupported("this firmware cannot receive an empty file "
                               "(its put command refuses a length of 0)")
        digest = hashlib.sha256(data).hexdigest()
        self.clear_line()
        self.type_line("put {} {} {}".format(quote(path), len(data), digest))
        text = self._read_until(b"raw bytes now", 10.0)
        if "raw bytes now" not in text:
            raise RpcError("the device never asked for the bytes:\n  " +
                           text.strip().replace("\n", "\n  "))
        sent = 0
        if progress:
            progress(0, len(data))
        while sent < len(data):
            block = data[sent:sent + PUT_CHUNK]
            self.t.write(block)
            sent += len(block)
            if not self._wait_ack(self.timing.legacy_put_ack):
                tail = self._read_until(b"\x00", 1.0)
                raise RpcError("no acknowledgement after {} of {} bytes{}".format(
                    sent, len(data), (":\n  " + tail.strip()) if tail.strip() else ""))
            if progress:
                progress(sent, len(data))
        # Success ends "sha256 <hex>"; a refusal says "was not kept" or "Could
        # not write". Whichever comes, the lines after it follow at once.
        report = self._read_until((b"sha256", b"was not kept", b"Could not write"), 30.0)
        report += self._read_until(b"\n\x00", 0.5)
        if "was not kept" in report:
            raise DigestMismatch("the device received something else; nothing was kept")
        if digest not in report:
            raise RpcError("the device did not confirm the file:\n  " +
                           report.strip().replace("\n", "\n  "))
        return digest

    def _wait_ack(self, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            b = self.t.read(0.2)
            if b and PUT_ACK in bytearray(b):
                return True
        return False

    def _read_until(self, needles, timeout):
        if isinstance(needles, bytes):
            needles = (needles,)
        buf = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            buf.extend(self.t.read(0.1))
            clean = strip_ansi(bytes(buf))
            if any(n in clean for n in needles):
                break
        return strip_ansi(bytes(buf)).decode("utf-8", "replace")


_PROMPT = re.compile(r"[\w.-]+@[\w.-]+:[^\n]*>\s*$")


def _output(text):
    """What the command printed: after the echo of the typed line, up to the
    marker.

    The echo is found by the quoted half of the marker ("RPCM") rather than by
    the token: the line editor may draw a grey suggestion from history in the
    middle of the token while it is typed, but the whole echo, suggestions and
    all, stays on one line."""
    lines = [ln.rstrip("\r") for ln in text.split("\n")]
    start = 0
    for i, ln in enumerate(lines):
        if '"RPCM" "' in ln:
            start = i + 1
            break
    out = [ln.split("\r")[-1] for ln in lines[start:]]
    tail = out.pop() if out else ""
    while out and _PROMPT.search(out[-1]):
        out.pop()
    body = "\n".join(out) + ("\n" if out else "")
    return body + tail
