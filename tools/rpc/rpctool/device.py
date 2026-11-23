"""A device, connected: link mode when the firmware has it, the prompt when not.

    from rpctool import Device
    with Device.open() as dev:              # finds the port, enters link mode
        print(dev.hello["os"], dev.hello["board"])
        r = dev.exec("ls /home")
        print(r.rc, r.text)
        dev.put(b"hello\\n", "/home/root/hi.txt")
        print(dev.get("/home/root/hi.txt"))

Leaving the `with` block (or close()) always returns the device to its normal
prompt, after an error or a Ctrl+C as well.
"""
import posixpath
import re
import time

from . import transport as _transport
from .errors import (DeviceGone, LinkError, LoginFailed, LoginRequired, NotSupported,
                     RpcError)
from .legacy import LegacyShell
from .link import ExecResult, Frame, FrameReader, LinkClient, Timing, _b64
from .transport import Transport, strip_ansi
from .util import quote, rnorm

_SHELL_PROMPT = re.compile(r"\S+@\S+:.*>\s*$")


def classify(data):
    """What the console is showing, from what it printed after a Ctrl+C:
    'shell', 'login', 'password', 'link', 'other' or None (nothing at all)."""
    text = strip_ansi(data).decode("utf-8", "replace")
    if "\x1e" in text:
        tail = text[text.rfind("\x1e"):]
        tail = tail.split("\n", 1)[1] if "\n" in tail else ""
        if not (_last_line(tail) and _SHELL_PROMPT.search(_last_line(tail))):
            return "link"
        text = tail
    last = _last_line(text)
    if "Username" in last:
        return "login"
    if "Password" in last:
        return "password"
    if _SHELL_PROMPT.search(last) or "^C" in text:
        return "shell"
    return "other" if text.strip() else None


def _last_line(text):
    return text.replace("\r\n", "\n").split("\n")[-1].split("\r")[-1]


class Device(object):
    """One connected device. Construct with a Transport, then connect()."""

    def __init__(self, transport, user=None, password=None, timing=None,
                 on_noise=None, sudo_password=None, bridge=None):
        self.t = transport
        # The network bridge (link protocol 3): an rpctool.bridge.Bridge that
        # answers the device's fetches, offered on connecting to firmware that
        # has no network of its own. None offers nothing.
        self.bridge = bridge
        self.user = user
        self.password = password          # a string, or a callable that asks
        # For sudo (link protocol 2): the signed-in account's password, a
        # string or a callable that asks. Asked for once, when the device first
        # says a request needs it, and kept for the rest of the connection.
        self.sudo_password = sudo_password
        self._sudo_known = None
        self.timing = timing or Timing()
        self.on_noise = on_noise
        self.mode = None                  # "link" or "legacy"
        self.link = None
        self.legacy = None
        self.gone = False

    # --- opening and closing ---------------------------------------------

    @classmethod
    def open(cls, port=None, **kw):
        path = _transport.choose_port(port)
        ser = _transport.open_serial(path)
        dev = cls(Transport(ser, path), **kw)
        try:
            return dev.connect()
        except BaseException:
            dev.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    @property
    def hello(self):
        return self.link.hello if self.link else {}

    @property
    def cwd(self):
        return self.hello.get("cwd", "/")

    def connect(self):
        """Get from whatever the console is doing to link mode (or, on firmware
        without it, to a clean prompt)."""
        t = self.t
        time.sleep(self.timing.settle / 2)
        t.discard_input()
        for _ in range(4):
            t.write(b"\x03")
            seen = self._collect(self.timing.settle)
            state = classify(seen)
            if state is None:
                # Nothing at all. A login prompt ignores Ctrl+C, so ask with an
                # empty line, which every prompt refuses without counting it.
                t.write(b"\r")
                seen = self._collect(self.timing.settle * 2)
                state = classify(seen)
            if state == "password":
                raise LoginRequired(
                    "The device is waiting for a password that something else started. "
                    "Finish or abandon that login in 'rpc term', then try again.")
            if state == "login":
                self._login()
                continue
            if state == "link":
                # Still in link mode from an earlier client: the Ctrl+C ended
                # its transfer, not link mode itself.
                self.link = LinkClient(t, FrameReader(t, self.on_noise), timing=self.timing)
                if self.link.resync():
                    self.mode = "link"
                    return self._linked()
                self.link = None
                continue
            t.write(b"link\r")
            what, rest = self._await_hello()
            if what == "hello":
                return self._linked()
            if what == "old":
                self.mode = "legacy"
                self.legacy = LegacyShell(t, self.timing, self.on_noise)
                return self
            if what == "login":
                self._login()
            # "link" (a stray frame) or nothing: go round again.
        raise RpcError("The device did not answer. Is the port right, and is the shell "
                       "running? 'rpc term' shows what it is doing.")

    def _linked(self):
        """In link mode: offer the bridge, where the firmware has it and this
        host was given one."""
        if self.bridge is not None:
            self.link.bridge = self.bridge
            try:
                self.link.bridge_on()
            except RpcError:
                pass              # the commands still run; they just have no network
        return self

    @property
    def bridged(self):
        return bool(self.link and self.link.bridged)

    def _collect(self, seconds):
        buf = bytearray()
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return bytes(buf)
            buf.extend(self.t.read(min(left, 0.1)))

    def _await_hello(self):
        buf = bytearray()
        end = time.monotonic() + self.timing.enter
        while time.monotonic() < end:
            buf.extend(self.t.read(0.1))
            at = buf.find(b"\x1eHELLO")
            if at >= 0:
                nl = buf.find(b"\n", at)
                if nl >= 0:
                    body = bytes(buf[at + 1:nl]).rstrip(b"\r")
                    reader = FrameReader(self.t, self.on_noise)
                    reader.feed(bytes(buf[nl + 1:]))
                    self.link = LinkClient(self.t, reader, timing=self.timing)
                    self.link.set_hello(Frame(body))
                    self.mode = "link"
                    return "hello", None
                continue
            clean = strip_ansi(bytes(buf))
            if b"'link' is not a command" in clean:
                self._collect(self.timing.settle)       # and the prompt after it
                return "old", None
            if b"Username" in _last_line(clean.decode("utf-8", "replace")).encode():
                return "login", None
            if b"\x1e" in buf and buf.rfind(b"\n") > buf.find(b"\x1e"):
                return "link", None
        return None, None

    def _type(self, text):
        data = text.encode("utf-8")
        for i in range(len(data)):
            self.t.write(data[i:i + 1])
            if self.timing.type_delay:
                time.sleep(self.timing.type_delay)
        self.t.write(b"\r")

    def _wait_for(self, patterns, timeout):
        """Read until one of the regexes matches the ANSI-free text; return the
        index of the first that did, or None."""
        buf = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            buf.extend(self.t.read(0.1))
            text = strip_ansi(bytes(buf)).decode("utf-8", "replace")
            for i, p in enumerate(patterns):
                if re.search(p, text, re.M):
                    return i
        return None

    def _login(self):
        if not self.user:
            raise LoginRequired(
                "The device is at a login prompt. Give an account with --user NAME "
                "(the password is asked for), or log in with 'rpc term' first.")
        self._type(self.user)
        i = self._wait_for([r"not found", r"Password[^\n]*$", r"Welcome|No password required"],
                           5.0)
        if i == 0:
            raise LoginFailed("The device has no account called '{}'.".format(self.user))
        if i == 1:
            pw = self.password() if callable(self.password) else self.password
            if not pw:
                raise LoginRequired("The account '{}' needs a password.".format(self.user))
            self._type(pw)
            j = self._wait_for([r"Incorrect password|Too many", r"Welcome"], 12.0)
            if j != 1:
                raise LoginFailed("The device refused the password for '{}'.".format(self.user))
        elif i is None:
            raise LoginFailed("No answer to the username.")
        self._collect(self.timing.settle)

    def close(self):
        """Back to the normal prompt, then let go of the port."""
        try:
            if not self.gone and self.mode == "link" and self.link:
                if self.link.busy:
                    kind, self.link.busy = self.link.busy, None
                    self.link.abort(kind)
                # Nobody here will answer a fetch once the port is closed.
                self.link.bridge_off()
                if not self.link.bye():
                    # Not in link mode after all, or not answering: a Ctrl+C
                    # leaves link mode, and at the prompt it is harmless.
                    self.t.write(b"\x03")
        except (RpcError, OSError):
            pass
        finally:
            self.t.close()

    # --- sudo --------------------------------------------------------------
    #
    # Firmware with link protocol 2 asks for sudo before it installs or removes
    # a package or changes anything in /os or /etc. A host tool proves the
    # password once with AUTH and the device remembers it for a few minutes.
    # Older firmware has no sudo at all, and is never asked.

    @property
    def proto(self):
        return self.link.proto if self.mode == "link" and self.link else 0

    @property
    def has_sudo(self):
        return self.proto >= 2

    def sudo_line(self, line):
        """`line` as it has to be sent to run with sudo on this firmware."""
        return "sudo " + line if self.has_sudo else line

    def _files_line(self, line, *paths):
        """A file command, with sudo when it touches /os or /etc: the device
        asks for it there, as it does for a PUT (and AUTH answers both)."""
        for p in paths:
            p = self.abspath(p)
            if p in ("/os", "/etc") or p.startswith("/os/") or p.startswith("/etc/"):
                return self.sudo_line(line)
        return line

    def _ask_sudo(self):
        pw = self._sudo_known
        if pw:
            return pw, True
        src = self.sudo_password
        pw = src() if callable(src) else src
        return pw, False

    def authenticate(self):
        """AUTH with the account's password, asking for it if need be (up to
        three times when it is typed). True when the device accepted one; False
        on firmware without sudo."""
        if not self.has_sudo:
            return False
        who = self.hello.get("user", "this account")
        for _ in range(3):
            pw, known = self._ask_sudo()
            if not pw:
                raise RpcError(
                    "This needs sudo on the device, which asks for {}'s password. Give it when "
                    "asked, in $RPC_PASSWORD, or say --sudo to be asked first.".format(who))
            try:
                self.link.auth(pw)
                self._sudo_known = pw
                return True
            except LinkError as e:
                if e.code != "auth":
                    raise
                self._sudo_known = None
                # A password that is not typed (a string, the environment)
                # will be just as wrong the second time.
                if known or not callable(self.sudo_password):
                    raise RpcError("The device refused the password for {}.".format(who))
        raise RpcError("Three wrong passwords for {}; nothing was done.".format(who))

    def _with_sudo(self, fn):
        """Run fn; if the device says it needs sudo, AUTH and run it again once."""
        try:
            return fn()
        except LinkError as e:
            if e.code != "sudo" or not self.has_sudo:
                raise
        self.authenticate()
        return fn()

    # --- what the firmware can do -------------------------------------------

    def _need_link(self, what):
        if self.mode != "link":
            raise NotSupported("{} needs link mode, which this firmware does not have. "
                               "Update the firmware ('rpc flash').".format(what))

    def exec(self, line, timeout=None):
        if self.mode == "link":
            return self._with_sudo(lambda: self.link.exec(line, timeout))
        rc, out, timed_out = self.legacy.run(line, timeout)
        return ExecResult(rc, out, False, timed_out)

    def check(self, line, timeout=None):
        """exec, raising RpcError with the device's own words when it fails."""
        r = self.exec(line, timeout)
        if r.rc != 0:
            raise RpcError(r.text.strip() or "'{}' failed".format(line))
        return r

    def put(self, data, path, progress=None):
        if self.mode == "link":
            return self._with_sudo(lambda: self.link.put(data, path, progress))
        return self.legacy.put(data, path, progress)

    # The file holding the password hashes, and the keys, need sudo to read on
    # protocol 2 firmware; the device says ERR sudo and AUTH gets them through.
    def get(self, path, progress=None):
        self._need_link("Fetching a file")
        return self._with_sudo(lambda: self.link.get(path, progress))

    def ls(self, path="."):
        self._need_link("Listing a directory")
        return self._with_sudo(lambda: self.link.ls(path))

    def stat(self, path):
        self._need_link("stat")
        return self._with_sudo(lambda: self.link.stat(path))

    def is_dir(self, path):
        st = self.stat(path)
        return bool(st and st[0] == "d")

    def abspath(self, path):
        return rnorm(path, self.cwd)

    def mkdir(self, path, parents=False):
        """Make a directory; with parents, every missing one above it too, and
        an existing directory is not an error."""
        path = self.abspath(path)
        if not parents:
            self.check(self._files_line("mkdir " + quote(path), path))
            return
        parts = [p for p in path.split("/") if p]
        cur = ""
        for p in parts:
            cur += "/" + p
            if self.mode == "link":
                st = self.stat(cur)
                if st is None:
                    self.check(self._files_line("mkdir " + quote(cur), cur))
                elif st[0] != "d":
                    raise RpcError("{} is a file, not a directory".format(cur))
            else:
                self.exec("mkdir " + quote(cur))     # an existing one fails; fine

    def rm(self, path, recursive=False):
        if "*" in path:
            raise RpcError("'{}': rm on the device treats * as a wildcard; refusing a name "
                           "that has one".format(path))
        return self.check(self._files_line(
            "rm {}{}".format("-r " if recursive else "", quote(path)), path))

    def mv(self, src, dst):
        return self.check(self._files_line("mv {} {}".format(quote(src), quote(dst)), src, dst))

    def walk(self, top):
        """(path, entries) for top and every directory below it, top first."""
        stack = [self.abspath(top)]
        while stack:
            d = stack.pop()
            ents = self.ls(d)
            yield d, ents
            for e in reversed(ents):
                if e.is_dir:
                    stack.append(posixpath.join(d, e.name))

    def reboot(self):
        """Restart the device. Returns None once it has gone, or the result of
        the command when the device refused (an account that is not admin)."""
        if self.mode == "link":
            self.link.send("EXEC reboot")
            out = bytearray()
            end = time.monotonic() + 4.0
            try:
                while time.monotonic() < end:
                    f = self.link.r.next_frame(0.3)
                    if f is None:
                        continue
                    if f.type == "OUT":
                        out.extend(_b64(f.rest))
                    elif f.final:
                        rc = int(f.kv().get("rc", "1")) if f.type == "OK" else 1
                        return ExecResult(rc, bytes(out))
            except DeviceGone:
                pass
        else:
            self.legacy.clear_line()
            self.legacy.type_line("reboot")
            try:
                self._collect(2.0)
            except DeviceGone:
                pass
        self.gone = True
        return None
