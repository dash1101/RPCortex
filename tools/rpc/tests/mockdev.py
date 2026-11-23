"""A pretend RPCortex v2 device, for testing the host tools with no board.

It runs the parts of the firmware a host tool meets, written from the source
rather than from memory:

  * the shell prompt and its line editor (os/core/lineedit.cpp): echo, a
    repaint now and then, Ctrl+C abandoning the line, printable ASCII only;
  * the login prompt (os/shell/session.cpp): Ctrl+C ignored, a blank answer
    refused without counting, bullets for a password;
  * link mode (LINK.md, os/shell/link.cpp): HELLO on entry with no OK, frames
    starting 0x1E, PUT with an ACK per D line, GET waiting for A, digest
    errors, CANCEL, Ctrl+C, the 10 s transfer timeout, LS, STAT, EXEC with
    captured output and rc;
  * the old `put` command with one 0x06 per 256 bytes (os/shell/shell.cpp);
  * firmware older than link mode, where `link` is not a command;
  * sudo (protocol 2, core/sudo.cpp): `pkg install` and `pkg remove` and any
    change in /os or /etc need it; `sudo` at the prompt asks for the password,
    in link mode it never asks and EXEC answers ERR sudo instead; AUTH starts
    the grace, which also lets PUT write into /os and /etc. proto=1 is the
    firmware from before, where none of that exists.
  * the network bridge (protocol 3, core/bridge.cpp and shell/bridge.cpp):
    with netless=True the device has no network of its own and says bridge=1;
    a host offers the bridge with BRIDGE on or the bytes 0x1E 0x1F (answered
    with a BRIDGE frame), the offer ends when the port closes, and then `wget`,
    `curl`, `pkg update` and `pkg install <name>` send FETCH frames in the
    middle of the command, at the prompt or in an EXEC, and read the host's
    answer one acknowledged chunk at a time. The input door drops stray 0x1E
    lines at the prompt. Without netless, BRIDGE answers ERR nobridge.

Output can end lines with CRLF, as the SDK's console does, and background
lines can be dropped between frames, as services do.

Two ways to connect: `dev.open()` gives an in-process object shaped like
pyserial's Serial, and `PtyMock` puts the device on a pseudo-terminal that real
pyserial opens by name (Linux and macOS).
"""
import base64
import binascii
import errno
import hashlib
import os
import posixpath
import random
import threading
import time

RS = b"\x1e"
LINE_MAX = 1100
CHUNK = 768
EXEC_MAX = 32768
PUT_CHUNK = 256

C_RESET = "\x1b[0m"
C_CYAN = "\x1b[36m"
C_FAIL = "\x1b[31m"
C_WARN = "\x1b[33m"
C_HEAD = "\x1b[35m"
C_WHITE = "\x1b[37m"
C_GRAY = "\x1b[90m"
C_BLUE = "\x1b[34m"

SYSTEM_DIRS = ("/", "/os", "/os/pkg", "/etc", "/home", "/tmp", "/usb")


class Closed(Exception):
    pass


class Reboot(Exception):
    pass


class Logout(Exception):
    pass


# --- the wire ---------------------------------------------------------------

class Pipe(object):
    """One direction of the wire."""

    def __init__(self):
        self.buf = bytearray()
        self.cv = threading.Condition()
        self.closed = False

    def write(self, data):
        with self.cv:
            self.buf.extend(data)
            self.cv.notify_all()

    def read(self, n, timeout):
        end = None if timeout is None else time.monotonic() + timeout
        with self.cv:
            while not self.buf:
                if self.closed:
                    raise Closed()
                left = None if end is None else end - time.monotonic()
                if left is not None and left <= 0:
                    return b""
                self.cv.wait(left if left is not None else 0.1)
            out = bytes(self.buf[:n])
            del self.buf[:n]
            return out

    def available(self):
        with self.cv:
            return len(self.buf)

    def clear(self):
        with self.cv:
            del self.buf[:]

    def close(self):
        with self.cv:
            self.closed = True
            self.cv.notify_all()


class FakeSerial(object):
    """The host's end of an in-process connection, shaped like serial.Serial."""

    def __init__(self, dev):
        self.dev = dev
        self.gen = dev.gen
        self.port = "mock"
        self.timeout = 0.05
        self.is_open = True
        self.dtr = True
        self.written = 0
        dev.host_opened(self)

    def _check(self):
        if not self.is_open:
            raise OSError(errno.EBADF, "port closed")
        if self.gen != self.dev.gen or self.dev.dead:
            raise OSError(errno.EIO, "device disconnected")

    @property
    def in_waiting(self):
        self._check()
        return self.dev.to_host.available()

    def read(self, size=1):
        self._check()
        try:
            data = self.dev.to_host.read(size, self.timeout)
        except Closed:
            raise OSError(errno.EIO, "device disconnected")
        self._check()
        return data

    def write(self, data):
        self._check()
        self.written += len(data)
        self.dev.from_host.write(bytes(data))
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.dev.to_host.clear()

    def close(self):
        if self.is_open:
            self.is_open = False
            self.dev.host_closed(self)


# --- the filesystem ---------------------------------------------------------

class MockFS(object):
    def __init__(self, capacity=1024 * 1024):
        self.files = {}
        self.dirs = set(SYSTEM_DIRS) | {"/home/root", "/home/guest"}
        self.capacity = capacity

    def isdir(self, p):
        return p in self.dirs

    def isfile(self, p):
        return p in self.files

    def exists(self, p):
        return self.isdir(p) or self.isfile(p)

    def used(self):
        return sum(len(v) for v in self.files.values())

    def free(self):
        return max(0, self.capacity - self.used())

    def listdir(self, d):
        out = []
        for p in self.dirs:
            if p != "/" and posixpath.dirname(p) == d:
                out.append((posixpath.basename(p), True, 0))
        for p, v in self.files.items():
            if posixpath.dirname(p) == d:
                out.append((posixpath.basename(p), False, len(v)))
        return sorted(out)

    def write(self, p, data):
        if not self.isdir(posixpath.dirname(p)) or self.isdir(p):
            return False
        self.files[p] = bytes(data)
        return True

    def remove(self, p):
        self.files.pop(p, None)

    def rmtree(self, p):
        for f in [f for f in self.files if f == p or f.startswith(p + "/")]:
            del self.files[f]
        for d in [d for d in self.dirs if d == p or d.startswith(p + "/")]:
            self.dirs.discard(d)

    def rename(self, a, b):
        if self.isfile(a):
            self.files[b] = self.files.pop(a)
            return True
        if self.isdir(a):
            for f in [f for f in self.files if f.startswith(a + "/")]:
                self.files[b + f[len(a):]] = self.files.pop(f)
            for d in [d for d in self.dirs if d == a or d.startswith(a + "/")]:
                self.dirs.discard(d)
                self.dirs.add(b + d[len(a):])
            return True
        return False


def resolve(cwd, path):
    if not path.startswith("/"):
        path = posixpath.join(cwd, path)
    path = posixpath.normpath(path)
    return "/" + path.lstrip("/")


def make_app(name, ver="1.0", size=600):
    """Bytes the mock's `pkg install` accepts as a package."""
    head = "RPCAPP {} {}\n".format(name, ver).encode()
    body = bytes(bytearray((i * 7 + len(name)) & 0xFF for i in range(max(0, size - len(head)))))
    return head + body


# --- the device -------------------------------------------------------------

class MockDevice(object):
    """Configure, then start(); then open() (in-process) or use PtyMock."""

    def __init__(self, link=True, login=False, crlf=True, noise=0.0, partial_noise=False,
                 seed=1, board="pico2_w", user="root", xfer_timeout=10.0, gone_after=3.0,
                 online=False, repaint=True, capacity=1024 * 1024, proto=2, netless=False):
        self.link_supported = link
        self.proto = proto if link else 1      # no link mode, no sudo either
        self.grace = False            # sudo's grace (AUTH, sudo -v, a typed password)
        self.elevated = False         # inside `sudo <command>`
        self.noprompt = False         # in an EXEC: sudo never asks
        self.sudo_blocked = False
        self.auths = []               # every password AUTH was given
        self.need_login = login
        self.crlf = crlf
        self.noise = noise
        self.partial_noise = partial_noise
        self.rng = random.Random(seed)
        self.board = board
        self.users = {"root": "rootpw", "guest": None}
        self.admins = {"root"}
        self.user = None if login else user
        self.cwd = "/"
        self.xfer_timeout = xfer_timeout
        self.gone_after = gone_after
        self.online = online
        self.repaint = repaint
        self.web = {}
        # the network bridge (protocol 3)
        self.netless = netless and self.proto >= 3
        self.bridge_offered = False
        self.bridge_id = 0
        self.bridge_quiet = 10.0      # BRIDGE_QUIET_MS
        self.bridge_fetches = []      # (method, url, status or error, bytes) per fetch
        self.bridge_frames = []       # every bridge frame the device sent
        self.bridge_stop_after = None # give up (FSTOP) after this many chunks
        self.bridge_chunk_delay = 0.0 # seconds per chunk, as a flash write takes
        self.repo_index = None        # pkg's repository index URL, fetched through the bridge
        self.raw_reading = False
        self.door = 0                 # 0 idle, 1 after 0x1E, 2 dropping to the line end
        self.typed_stray = []         # what the door dropped
        self.installed = {"greet": "1.0"}
        self.fs = MockFS(capacity)
        self._seed_files()

        # faults
        self.corrupt_put = False      # the device hashes what it got, after a bit flip
        self.corrupt_get = False      # a D frame is damaged on the way to the host
        self.stall_get = 0.0          # seconds of silence before the second D frame
        self.drop_link = False        # leave link mode silently after the next request

        # what happened, for the tests to look at
        self.requests = []            # every link-mode request line
        self.login_typed = []         # every answer typed at a login prompt
        self.typed_lines = []         # every line run at the prompt
        self.in_link = False
        self.link_entries = 0
        self.byes = 0
        self.cancels = 0

        self.gen = 0
        self.dead = False
        self.to_host = Pipe()
        self.from_host = Pipe()
        self.host = None
        self.dtr = False
        self.dtr_lost = None
        self.capture = None
        self.had_error = False
        self.thread = None

    def _seed_files(self):
        fs = self.fs
        fs.files["/os/registry.cfg"] = b"System.Setup=true\n"
        fs.files["/os/users.cfg"] = b"root:salted:hash\n"
        fs.files["/os/device.key"] = b"\x01" * 32
        fs.files["/os/pkg/greet.app"] = make_app("greet")
        fs.files["/os/pkg/index.cfg"] = b"greet 1.0\n"
        fs.files["/etc/startup.cfg"] = b"# startup\n"
        fs.files["/home/root/notes.txt"] = b"hello from the device\n"
        fs.files["/home/root/secret.txt"] = b"root only\n"
        fs.files["/usb/readme.txt"] = b"drop files here\n"

    # --- running ----------------------------------------------------------

    def start(self):
        self.thread = threading.Thread(target=self._main, name="mockdev")
        self.thread.daemon = True
        self.thread.start()
        return self

    def open(self):
        return FakeSerial(self)

    def shutdown(self):
        self.dead = True
        self.from_host.close()
        self.to_host.close()
        if self.thread:
            self.thread.join(2.0)

    def host_opened(self, s):
        self.host = s
        self.dtr = True
        self.dtr_lost = None

    def host_closed(self, s):
        if self.host is s:
            self.dtr = False
            self.dtr_lost = time.monotonic()
            self.bridge_offered = False   # tud_cdc_line_state_cb: DTR dropped

    def _main(self):
        try:
            while not self.dead:
                try:
                    if self.user is None:
                        self.session_boot()
                    self.shell()
                except Logout:
                    self.user = None
                except Reboot:
                    self._reboot()
        except Closed:
            pass

    def _reboot(self):
        self.gen += 1
        self.to_host.clear()
        self.from_host.clear()
        self.in_link = False
        self.cwd = "/"
        if self.need_login:
            self.user = None
        time.sleep(0.05)

    # --- raw I/O ----------------------------------------------------------

    def emit(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8", "surrogateescape")
        if self.crlf:
            data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        self.to_host.write(data)

    def getkey(self, timeout):
        end = None if timeout is None else time.monotonic() + timeout
        while True:
            left = None if end is None else max(0.0, end - time.monotonic())
            b = self.from_host.read(1, left)
            if not b:
                return None
            c = b[0]
            if self.raw_reading or self.proto < 3 or not self.netless:
                return c
            r = self._door(c)
            if r is not None:
                return r

    def _door(self, c):
        """core/bridge.cpp's bridge_door: the offer and stray 0x1E lines are not keys."""
        if c == 3:
            self.door = 0
            return c
        if self.door == 0:
            if c == 0x1E:
                self.door = 1
                return None
            return c
        if self.door == 1:
            if c == 0x1F:
                self.door = 0
                self.bridge_offered = True
                self.emit(RS + "BRIDGE on chunk={}".format(CHUNK).encode() + b"\n")
                return None
            self.door = 0 if c in (10, 13) else 2
            return None
        if c in (10, 13):
            self.door = 0
        self.typed_stray.append(c)
        return None

    # --- output, as os/core/out.cpp writes it -----------------------------

    def _tagged(self, colour, sym, msg, prefix=None):
        if self.capture is not None:
            self.capture.extend("[{}] {}{}\n".format(
                sym, "[{}] ".format(prefix) if prefix else "", msg).encode("utf-8"))
        else:
            head = "{}[{}{}{}]".format(colour, C_WHITE, sym, colour)
            if prefix:
                head += " {}[{}{}{}]".format(colour, C_WHITE, prefix, colour)
            self.emit("{} {}{}\n".format(head, C_RESET, msg))

    def out_ok(self, msg, prefix=None):
        self._tagged(C_CYAN, "@", msg, prefix)

    def out_info(self, msg, prefix=None):
        self._tagged(C_HEAD, ":", msg, prefix)

    def out_warn(self, msg, prefix=None):
        self._tagged(C_WARN, "?", msg, prefix)

    def out_err(self, msg, prefix=None):
        self.had_error = True
        self._tagged(C_FAIL, "!", msg, prefix)

    def out(self, text):
        if isinstance(text, str):
            text = text.encode("utf-8", "surrogateescape")
        if self.capture is not None:
            self.capture.extend(text)
        else:
            self.emit(text)

    # --- login ------------------------------------------------------------

    def read_field(self, prompt, secret=False):
        self.emit("{}{} {}\xe2\x80\xa2\xe2\x80\xa2>  {}".format(C_RESET, prompt, C_CYAN, C_RESET)
                  .encode("latin-1"))
        buf = bytearray()
        while True:
            c = self.getkey(None)
            if c is None:
                continue
            if c in (13, 10):
                self.emit("\n")
                return buf.decode("ascii")
            if c in (8, 127):
                if buf:
                    del buf[-1]
                    self.emit("\b \b")
                continue
            if 32 <= c < 127 and len(buf) < 39:
                buf.append(c)
                self.emit(b"\xe2\x80\xa2" if secret else bytes([c]))

    def session_boot(self):
        self.out_info("=== Login ===")
        self.out("  Type 'root' or 'guest' to log in.\n\n")
        while True:
            name = self.read_field("Username")
            if not name:
                self.out_warn("Please enter a username.")
                continue
            self.login_typed.append(name)
            if name not in self.users:
                self.out_warn("User '{}' not found.".format(name))
                self.out("  Available accounts: root, guest\n")
                continue
            if self.users[name] is None:
                self.out_info("No password required for '{}'.".format(name))
                return self._accept(name)
            attempts = 0
            while True:
                pw = self.read_field("Password", True)
                if not pw:
                    self.out_warn("Password cannot be blank.")
                    continue
                self.login_typed.append("<password>")
                if pw == self.users[name]:
                    return self._accept(name)
                attempts += 1
                if attempts < 3:
                    self.out_warn("Incorrect password.  Attempt {}/3.".format(attempts))
                else:
                    self.out_err("Too many failed attempts.  Returning to username prompt.")
                    break

    def _accept(self, name):
        self.user = name
        self.cwd = "/home/" + name if self.fs.isdir("/home/" + name) else "/"
        self.out_ok("Welcome, {}!".format(name))

    # --- the prompt -------------------------------------------------------

    def prompt(self):
        return "{}{}{}@{}vela{}:{}{}{}{}>{} ".format(
            C_CYAN, self.user, C_GRAY, C_RESET, "", C_BLUE, self.cwd, C_RESET, C_CYAN, C_RESET)

    def read_line(self):
        prompt = self.prompt()
        self.emit(prompt)
        buf = bytearray()
        while True:
            c = self.getkey(None)
            if c is None:
                continue
            if c in (13, 10):
                self.emit("\n")
                return buf.decode("ascii")
            if c == 3:
                self.emit("^C\n" + prompt)
                del buf[:]
                continue
            if c in (8, 127):
                if buf:
                    del buf[-1]
                    self.emit("\b \b")
                continue
            if c == 0x1B:
                k = self.getkey(0.05)
                while k is not None and not (0x40 <= k <= 0x7E and k != ord("[")):
                    k = self.getkey(0.05)
                continue
            if 32 <= c < 127 and len(buf) < 127:      # char line[128] in shell.cpp
                buf.append(c)
                self.emit(bytes([c]))
                if self.repaint and self.rng.random() < 0.15:
                    # a full repaint, as a suggestion or a mid-line edit causes
                    self.emit("\r" + prompt + buf.decode("ascii") + "\x1b[K")

    def shell(self):
        while True:
            line = self.read_line()
            self.typed_lines.append(line)
            self.run_line(line)

    # --- running lines (cmdline.cpp's quoting rules) ----------------------

    @staticmethod
    def split_connectors(line):
        segs, cur, kind = [], [], "first"
        quote = None
        i, n = 0, len(line)
        while i < n:
            ch = line[i]
            if quote:
                cur.append(ch)
                if ch == quote:
                    quote = None
                i += 1
                continue
            if ch in "'\"" and (not cur or cur[-1] == " "):
                quote = ch
                cur.append(ch)
                i += 1
                continue
            two = line[i:i + 2]
            if two in ("&&", "||") or ch == ";":
                segs.append((kind, "".join(cur).strip()))
                kind = {"&&": "and", "||": "or"}.get(two, "seq")
                cur = []
                i += 2 if two in ("&&", "||") else 1
                continue
            cur.append(ch)
            i += 1
        segs.append((kind, "".join(cur).strip()))
        return segs

    @staticmethod
    def split_args(seg):
        args, i, n = [], 0, len(seg)
        while i < n:
            while i < n and seg[i] == " ":
                i += 1
            if i >= n:
                break
            word = ""
            if seg[i] in "'\"":
                q = seg[i]
                j = seg.find(q, i + 1)
                if j < 0:
                    word, i = seg[i + 1:], n
                else:
                    word, i = seg[i + 1:j], j + 1
            while i < n and seg[i] != " ":
                word += seg[i]
                i += 1
            args.append(word)
        return args

    def run_line(self, line):
        status = 0
        for kind, seg in self.split_connectors(line):
            if (kind == "and" and status != 0) or (kind == "or" and status == 0):
                continue
            if not seg:
                continue
            self.had_error = False
            args = self.split_args(seg)
            status = self.run_command(args)
            if status == 0 and self.had_error:
                status = 1
        return status

    def run_command(self, args):
        name = args[0]
        if name == "sudo":
            return self.c_sudo(args)
        fn = getattr(self, "c_" + name, None)
        if name == "link" and not self.link_supported:
            fn = None
        if fn is None:
            self.out_err("'{}' is not a command or executable file.".format(name))
            return 1
        return fn(args) or 0

    # --- permissions ------------------------------------------------------

    @staticmethod
    def is_system(p):
        return p in ("/os", "/etc") or p.startswith("/os/") or p.startswith("/etc/")

    def needs_sudo(self, p):
        """An admin changing /os or /etc without sudo (protocol 2 only)."""
        return (self.proto >= 2 and self.user in self.admins and self.is_system(p)
                and not self.elevated)

    def read_needs_sudo(self, p):
        """An admin reading the password hashes or a key without sudo."""
        return (self.proto >= 2 and self.user in self.admins and not self.elevated
                and (p in ("/os/users.cfg", "/os/users.bak", "/os/device.key")
                     or p == "/os/ssh" or p.startswith("/os/ssh/")))

    def sudo_auth(self, what="use sudo"):
        if self.elevated or self.grace:
            return True
        if self.noprompt:
            self.sudo_blocked = True
            self.out_err("sudo needs a password and cannot ask for one here; run `sudo -v` first.")
            return False
        for _ in range(3):
            pw = self.read_field("Password for {}, to {}".format(self.user, what), secret=True)
            if pw and pw == self.users.get(self.user):
                self.grace = True
                return True
            self.out_err("Wrong password.")
        return False

    def c_sudo(self, a):
        if len(a) < 2:
            self.out("Usage: sudo <command> [args...]\n")
            return 1
        if self.proto < 2:
            # Before sudo asked for anything: an admin's word was enough.
            if self.user not in self.admins:
                self.out_err("{} is not an admin. sudo cannot grant what the account does not have."
                             .format(self.user))
                return 1
            return self.run_command(a[1:])
        if a[1] == "-k":
            self.grace = False
            return 0
        if self.user not in self.admins:
            self.out_err("{} is not an admin. sudo cannot grant what the account does not have."
                         .format(self.user))
            return 1
        if a[1] == "-v":
            return 0 if self.sudo_auth() else 1
        if not self.sudo_auth("run '{}'".format(a[1])):
            return 1
        was, self.elevated = self.elevated, True
        try:
            return self.run_command(a[1:])
        finally:
            self.elevated = was

    def may_read(self, p):
        if self.user == "root":
            return True
        return not (p in ("/os/users.cfg", "/os/device.key") or p.startswith("/home/root"))

    def may_change(self, p):
        if self.user == "root":
            return not self.needs_sudo(p)
        return p.startswith("/home/{}/".format(self.user)) or p.startswith("/tmp/")

    # --- commands ---------------------------------------------------------

    def c_echo(self, a):
        self.out(" ".join(a[1:]) + "\n")

    def c_whoami(self, a):
        self.out(self.user + "\n")

    def c_ver(self, a):
        self.out("RPCortex v2.01-beta (build 554)\nGCC 14.2.1   Platform: {} ({})\n"
                 .format(self.board, "RP2350" if self.board.startswith("pico2") else "RP2040"))

    def c_ls(self, a):
        p = resolve(self.cwd, a[1] if len(a) > 1 else ".")
        if not self.may_read(p):
            self.out_err("Permission denied: {}".format(p))
            return 1
        if not self.fs.isdir(p):
            self.out_err("No such directory: {}".format(p))
            return 1
        for name, d, size in self.fs.listdir(p):
            self.out("  {}  {:>7}  {}\n".format("DIR " if d else "FILE", "-" if d else size, name))

    def c_cat(self, a):
        if len(a) < 2:
            self.out("Usage: cat <file>\n")
            return 1
        p = resolve(self.cwd, a[1])
        if not self.fs.isfile(p):
            self.out_err("No such file: {}".format(p))
            return 1
        self.out(self.fs.files[p])

    def c_mkdir(self, a):
        if len(a) < 2:
            self.out("Usage: mkdir <dir>\n")
            return 1
        p = resolve(self.cwd, a[1])
        if not self.may_change(p):
            self.out_err("Permission denied: {}".format(p))
            return 1
        if self.fs.exists(p) or not self.fs.isdir(posixpath.dirname(p)):
            self.out_err("Could not create {}".format(p))
            return 1
        self.fs.dirs.add(p)

    def c_rm(self, a):
        rec = False
        rest = a[1:]
        while rest and rest[0] in ("-r", "-rf"):
            rec = True
            rest = rest[1:]
        if not rest:
            self.out("Usage: rm [-r] <path>...\n")
            return 1
        for arg in rest:
            p = resolve(self.cwd, arg)
            targets = [p]
            if "*" in posixpath.basename(p):
                pre, _, suf = posixpath.basename(p).partition("*")
                d = posixpath.dirname(p)
                targets = [posixpath.join(d, n) for n, _, _ in self.fs.listdir(d)
                           if n.startswith(pre) and n.endswith(suf)]
            for t in targets:
                if t in SYSTEM_DIRS:
                    self.out_err("{} is a system directory and stays.".format(t))
                    continue
                if not self.may_change(t):
                    self.out_err("Permission denied: {}".format(t))
                elif self.fs.isfile(t):
                    self.fs.remove(t)
                elif self.fs.isdir(t):
                    if not rec:
                        self.out_err("{} is a directory; rm -r removes it".format(t))
                    else:
                        self.fs.rmtree(t)
                else:
                    self.out_err("No such file: {}".format(t))

    def c_mv(self, a):
        if len(a) < 3:
            self.out("Usage: mv <from> <to>\n")
            return 1
        x, y = resolve(self.cwd, a[1]), resolve(self.cwd, a[2])
        if self.fs.isdir(y):
            y = posixpath.join(y, posixpath.basename(x))
        if x in SYSTEM_DIRS or not self.may_change(x) or not self.may_change(y) \
                or not self.fs.rename(x, y):
            self.out_err("Could not move {}".format(x))
            return 1

    def c_cd(self, a):
        p = resolve(self.cwd, a[1] if len(a) > 1 else "/")
        if not self.fs.isdir(p):
            self.out_err("No such directory: {}".format(p))
            return 1
        self.cwd = p

    def c_meminfo(self, a):
        self.out_info("Memory")
        self.out("  Total : 480 KB\n  Used  : 200 KB  (41%)\n  Free  : 280 KB\n"
                 "  Largest block : 180 KB\n")

    def c_df(self, a):
        total = self.fs.capacity // 1024
        used = self.fs.used() // 1024
        self.out_info("Filesystem")
        self.out("  Total : {} KB\n  Used  : {} KB  ({}%)\n  Free  : {} KB\n".format(
            total, used, used * 100 // max(total, 1), total - used))

    def c_ps(self, a):
        self.out("  PID  STATE  CORE  NAME\n    1  wait   0     init\n    2  run    0     shell\n"
                 "2 tasks  CPU 3%\n")

    def c_logdump(self, a):
        self.out("[0.412] boot ok\n[1.020] [:] link ready\n")

    def c_reboot(self, a):
        if self.user not in self.admins:
            self.out_err("'reboot' needs an admin account.")
            return 1
        self.out_info("Rebooting system...")
        raise Reboot()

    def c_logout(self, a):
        raise Logout()

    def c_fail(self, a):
        self.out_err("it failed")
        return 0

    def c_big(self, a):
        n = int(a[1]) if len(a) > 1 else EXEC_MAX + 1000
        line = b"0123456789abcdef" * 4 + b"\n"
        self.out((line * (n // len(line) + 1))[:n])

    def c_sleep(self, a):
        end = time.monotonic() + float(a[1] if len(a) > 1 else 1)
        while time.monotonic() < end:
            if self.getkey(0.01) == 3:
                self.out_warn("Interrupted.")
                return 130

    def c_ask(self, a):
        self.out("Are you sure? [y/N] ")
        while True:
            c = self.getkey(None)
            if c == 3:
                return 1
            if c in (ord("y"), ord("Y")):
                return 0

    # --- the network bridge (protocol 3) ----------------------------------

    def bridged(self):
        return self.netless and self.bridge_offered

    def no_route(self):
        if self.netless:
            self.out_err("This board has no wireless hardware.")
            self.out("  With rpc or the browser tool attached, this goes through the computer's\n"
                     "  connection: 'rpc term', 'rpc exec', or the browser tool's terminal.\n")
        else:
            self.out_err("No network. Connect with 'wifi connect <ssid>' first.")
        return 1

    def bridge_frame(self, text):
        self.bridge_frames.append(text)
        self.emit(RS + text.encode("utf-8", "surrogateescape") + b"\n")

    def bridge_line(self, quiet, stoppable=True):
        """('ok', type, id, rest) | ('stop',) | ('quiet',)"""
        buf, inframe, last = bytearray(), False, time.monotonic()
        while True:
            c = self.getkey(0.01)
            if c is None:
                if time.monotonic() - last >= quiet:
                    return ("quiet",)
                continue
            last = time.monotonic()
            if c == 3:
                if stoppable:
                    return ("stop",)
                continue
            if c == 0x1E:
                buf, inframe = bytearray(), True
                continue
            if not inframe or c == 13:
                continue
            if c == 10:
                inframe = False
                parts = bytes(buf).decode("utf-8", "replace").split(" ", 2)
                if len(parts) >= 2 and parts[1].isdigit():
                    return ("ok", parts[0], int(parts[1]), parts[2] if len(parts) > 2 else "")
                continue
            buf.append(c)

    def bridge_drain(self, rid, stop=True):
        if stop:
            self.bridge_frame("FSTOP {}".format(rid))
        end = time.monotonic() + 3.0
        while time.monotonic() < end:
            r = self.bridge_line(max(0.01, end - time.monotonic()), stoppable=False)
            if r[0] != "ok":
                return
            if r[1] == "FDONE" and r[2] == rid:
                return

    def bridge_fetch(self, method, url, headers=(), body=b""):
        """(status, body, None) or (None, None, why), the way core/bridge.cpp
        and httpfetch.cpp between them answer a command."""
        self.bridge_id += 1
        rid = self.bridge_id
        self.bridge_frame("FETCH {} {} {}".format(rid, method, url))
        for h in headers:
            self.bridge_frame("FHDR {} {}".format(rid, base64.b64encode(h.encode()).decode()))
        for i in range(0, len(body), CHUNK):
            self.bridge_frame("FBODY {} {}".format(rid, base64.b64encode(body[i:i + CHUNK]).decode()))
        self.bridge_frame("FGO {} chunk={}".format(rid, CHUNK))
        self.raw_reading = True
        status, length, data, chunks = None, None, bytearray(), 0
        try:
            while True:
                r = self.bridge_line(self.bridge_quiet)
                if r[0] == "quiet":
                    self.bridge_offered = False
                    why = "the computer did not answer for {:.0f} s".format(self.bridge_quiet)
                    self.bridge_fetches.append((method, url, "quiet", 0))
                    return None, None, why
                if r[0] == "stop":
                    self.bridge_drain(rid)
                    self.bridge_fetches.append((method, url, "cancelled", len(data)))
                    return None, None, "cancelled"
                _, typ, fid, rest = r
                if fid != rid:
                    continue
                if typ == "FHEAD":
                    status = int(rest.split(" ")[0])
                    for w in rest.split(" ")[1:]:
                        if w.startswith("len="):
                            length = int(w[4:])
                elif typ == "FDATA":
                    data.extend(base64.b64decode(rest))
                    chunks += 1
                    if self.bridge_stop_after is not None and chunks >= self.bridge_stop_after:
                        self.bridge_drain(rid)
                        self.bridge_fetches.append((method, url, "stopped", len(data)))
                        return None, None, "too large"
                    if self.bridge_chunk_delay:
                        time.sleep(self.bridge_chunk_delay)
                    self.bridge_frame("FACK {} {}".format(rid, len(data)))
                elif typ == "FDONE":
                    if rest.startswith("err"):
                        msg = rest.split(" ", 2)[2] if len(rest.split(" ", 2)) > 2 else rest
                        self.bridge_fetches.append((method, url, rest.split(" ")[1], 0))
                        return None, None, msg
                    if length is not None and len(data) != length:
                        return None, None, "truncated after {} bytes".format(len(data))
                    self.bridge_fetches.append((method, url, status, len(data)))
                    return status, bytes(data), None
        finally:
            self.raw_reading = False

    def get_url(self, url):
        """(body, None) or (None, why): over the bridge when the device has no
        network of its own, from self.web when it is online."""
        if self.online:
            if url not in self.web:
                return None, "HTTP 404"
            return self.web[url], None
        if not self.bridged():
            return None, None
        status, body, why = self.bridge_fetch("GET", url)
        if why:
            return None, why
        if not 200 <= status < 300:
            return None, "server refused - HTTP {}".format(status)
        return body, None

    def c_curl(self, a):
        url = a[-1] if len(a) > 1 else None
        if not url or url.startswith("-"):
            self.out("Usage: curl <url>\n")
            return 1
        if not (self.online or self.bridged()):
            return self.no_route()
        body, why = self.get_url(url)
        if body is None:
            self.out_err(why or "failed")
            return 1
        self.out(body + (b"" if body.endswith(b"\n") else b"\n"))
        return 0

    def c_wget(self, a):
        if len(a) < 2:
            self.out("Usage: wget <url> [file]\n")
            return 1
        if self.netless:
            if not self.bridged():
                return self.no_route()
            self.out_info("Fetching {}".format(a[1]))
            body, why = self.get_url(a[1])
            if body is None:
                self.out_err(why)
                return 1
            dest = resolve(self.cwd, a[2] if len(a) > 2 else posixpath.basename(a[1]))
            self.fs.write(dest, body)
            self.out_ok("Saved {} ({} bytes)".format(dest, len(body)))
            return 0
        if not self.online:
            self.out_err("No network. Connect with 'wifi connect <ssid>' first.")
            return 1
        if a[1] not in self.web:
            self.out_err("HTTP 404")
            return 1
        dest = resolve(self.cwd, a[2] if len(a) > 2 else posixpath.basename(a[1]))
        self.fs.write(dest, self.web[a[1]])
        self.out_ok("Saved {} ({} bytes)".format(dest, len(self.web[a[1]])))

    def c_pkg(self, a):
        sub = a[1] if len(a) > 1 else ""
        if sub == "list":
            self.out_info("Installed packages:")
            for k in sorted(self.installed):
                self.out("  {:<14} {}\n".format(k, self.installed[k]))
            return 0
        if self.proto >= 2 and sub in ("install", "remove") and not self.elevated:
            self.out_err("'pkg {}' changes the system. Run it with sudo: sudo {}".format(
                sub, " ".join(a)))
            return 1
        if sub == "remove" and len(a) > 2:
            if a[2] not in self.installed:
                self.out_err("'{}' is not installed.".format(a[2]))
                return 1
            del self.installed[a[2]]
            self.fs.files.pop("/os/pkg/{}.app".format(a[2]), None)
            self.out_ok("Removed {}.".format(a[2]), "pkg")
            return 0
        if sub == "install" and len(a) > 2 and "/" not in a[2] and self.netless:
            return self.pkg_install_bridged(a[2])
        if sub == "install" and len(a) > 2:
            arg = a[2]
            if "/" not in arg:
                if not self.online:
                    self.out_err("No network. Connect with 'wifi connect <ssid>' first.")
                    return 1
            p = resolve(self.cwd, arg)
            if not self.fs.isfile(p):
                self.out_err("No such file: {}".format(p))
                return 1
            data = self.fs.files[p]
            if not data.startswith(b"RPCAPP "):
                self.out_err("Not a valid package: bad magic")
                return 1
            name, ver = data.split(b"\n", 1)[0].decode().split(" ")[1:3]
            self.fs.files["/os/pkg/{}.app".format(name)] = data
            self.installed[name] = ver
            self.out_ok("Installed {} {}".format(name, ver), "pkg")
            return 0
        self.out("Usage: pkg install|list ...\n")
        return 1

    def pkg_install_bridged(self, name):
        """pkgfetch.cpp: the index, then the package, both through the bridge,
        and the package's digest checked against the index."""
        if not self.bridged():
            return self.no_route()
        import json
        idx, why = self.get_url(self.repo_index or "https://repo.invalid/index.json")
        if idx is None:
            self.out_err(why)
            return 1
        try:
            entries = json.loads(idx.decode("utf-8")).get("packages", [])
        except ValueError:
            self.out_err("nothing readable at this repo")
            return 1
        e = next((x for x in entries if x.get("name") == name), None)
        if e is None:
            self.out_err("No package called '{}'.".format(name))
            return 1
        self.out_info("Installing {} {}".format(name, e.get("ver", "")))
        data, why = self.get_url(e["url"])
        if data is None:
            self.out_err(why)
            return 1
        if e.get("sha256") and hashlib.sha256(data).hexdigest() != e["sha256"]:
            self.out_err("Checksum mismatch - the download does not match the index.")
            return 1
        if not data.startswith(b"RPCAPP "):
            self.out_err("The package downloaded but would not install.")
            return 1
        ver = data.split(b"\n", 1)[0].decode().split(" ")[2]
        self.fs.files["/os/pkg/{}.app".format(name)] = data
        self.installed[name] = ver
        self.out_ok("Installed {} {} ({} bytes).".format(name, ver, len(data)))
        return 0

    # --- the old put ------------------------------------------------------

    def c_put(self, a):
        if len(a) < 3:
            self.out("Usage: put <name> <len> [sha256]\n")
            return 1
        length = int(a[2])
        want = a[3] if len(a) > 3 else None
        if length == 0:
            self.out_err("Invalid length.")
            return 1
        path = resolve(self.cwd, a[1])
        self.out_info("Send {} raw bytes now...".format(length))
        got = bytearray()
        while len(got) < length:
            b = self.from_host.read(min(PUT_CHUNK - len(got) % PUT_CHUNK, length - len(got)), 2.0)
            if not b:
                self.out_err("Timed out after {} of {} bytes.".format(len(got), length))
                return 1
            got.extend(b)
            if len(got) % PUT_CHUNK == 0 or len(got) == length:
                self.to_host.write(b"\x06")
        if self.corrupt_put:
            got[0] ^= 0x01
        hexd = hashlib.sha256(bytes(got)).hexdigest()
        if want and want != hexd:
            self.out_err("What arrived is not what was sent — {} was not kept.".format(path))
            self.out("  expected  {}\n  received  {}\n".format(want, hexd))
            return 1
        self.fs.write(path, bytes(got))
        self.out_ok("Wrote {} ({} bytes).".format(path, length))
        self.out("  sha256 {}{}\n".format(hexd, "  (matches)" if want else ""))

    # --- link mode --------------------------------------------------------

    def frame(self, text):
        if self.noise and self.rng.random() < self.noise:
            if self.partial_noise and self.rng.random() < 0.5:
                self.emit("{}[{}:{}]{} [svc] half a line ".format(C_HEAD, C_WHITE, C_HEAD, C_RESET))
            else:
                self.emit("{}[{}:{}]{} [svc] background tick {}\n".format(
                    C_HEAD, C_WHITE, C_HEAD, C_RESET, self.rng.randint(0, 999)))
        if isinstance(text, str):
            text = text.encode("utf-8", "surrogateescape")
        self.emit(RS + text + b"\n")

    def err(self, code, msg):
        self.frame("ERR {} {}".format(code, msg))

    def link_read(self, timeout, watch=False):
        """('ok', line) | ('timeout',) | ('intr',) | ('long',) | ('gone',)"""
        buf = bytearray()
        over = False
        last = time.monotonic()
        while True:
            c = self.getkey(0.01)
            now = time.monotonic()
            if c is None:
                if timeout and now - last >= timeout:
                    return ("timeout", None)
                if watch and not self.dtr and self.dtr_lost and now - self.dtr_lost >= self.gone_after:
                    return ("gone", None)
                continue
            last = now
            if c == 3:
                return ("intr", None)
            if c == 13:
                continue
            if c == 10:
                if over:
                    return ("long", None)
                return ("ok", bytes(buf).decode("utf-8", "surrogateescape"))
            if len(buf) + 1 < LINE_MAX:
                buf.append(c)
            else:
                over = True

    def hello(self):
        cwd = self.cwd.replace("%", "%25").replace(" ", "%20")
        # Firmware from 2026-11-20 says arch; earlier link firmware did not
        # (send_arch = False), and an unknown board then needs --arch.
        arch = ""
        if getattr(self, "send_arch", True):
            arch = "arch={} ".format("armv6m" if self.board in ("pico", "pico_w") else "armv8m")
        bridge = " bridge={}".format(1 if self.netless else 0) if self.proto >= 3 else ""
        self.frame("HELLO proto={} os=v2.01-beta build=554 board={} {}user={} cwd={} chunk={} "
                   "exec_max={}{}".format(self.proto, self.board, arch, self.user, cwd, CHUNK,
                                          EXEC_MAX, bridge))

    def c_link(self, a):
        if self.in_link:
            self.out_err("Link mode is already running.")
            return 1
        self.emit("Link mode, for host tools (LINK.md). Ctrl+C returns to the prompt.\n")
        self.in_link = True
        self.link_entries += 1
        try:
            self.hello()
            while True:
                r = self.link_read(0, watch=True)
                if r[0] in ("intr", "gone"):
                    break
                if r[0] == "long":
                    self.err("badreq", "line too long")
                    continue
                line = r[1]
                if not line:
                    continue
                self.requests.append(line)
                verb, _, rest = line.partition(" ")
                if not verb or len(verb) >= 8:
                    self.err("badreq", "no verb")
                    continue
                if verb == "PING":
                    self.frame("OK")
                elif verb == "HELLO":
                    self.hello()
                    self.frame("OK")
                elif verb == "EXEC":
                    self.l_exec(rest)
                elif verb == "PUT":
                    self.l_put(rest)
                elif verb == "GET":
                    self.l_get(rest)
                elif verb == "LS":
                    self.l_ls(rest)
                elif verb == "STAT":
                    self.l_stat(rest)
                elif verb == "AUTH" and self.proto >= 2:
                    self.l_auth(rest)
                elif verb == "BRIDGE" and self.proto >= 3:
                    self.l_bridge(rest)
                elif verb == "BYE":
                    self.byes += 1
                    self.frame("OK")
                    break
                else:
                    self.err("badreq", "unknown verb " + verb)
                if self.drop_link:
                    self.drop_link = False
                    break
        finally:
            self.in_link = False
        self.frame("BYE")          # the firmware says so however link mode ends
        self.emit("Left link mode.\n")
        return 0

    def _path(self, rest, change):
        if not rest:
            self.err("badreq", "a path is needed")
            return None
        p = resolve(self.cwd, rest)
        if len(p.encode("utf-8", "surrogateescape")) >= 200:
            self.err("toolong", "path too long")
            return None
        if (self.needs_sudo(p) if change else self.read_needs_sudo(p)):
            self.err("sudo", "{} needs sudo: send AUTH first".format(p))
            return None
        if not (self.may_change(p) if change else self.may_read(p)):
            self.err("perm", "{} is protected for this account".format(p))
            return None
        return p

    def l_exec(self, line):
        if not line:
            return self.err("badreq", "EXEC needs a command")
        self.capture = bytearray()
        self.noprompt, self.sudo_blocked = True, False
        try:
            rc = self.run_line(line)
        except Logout:
            rc = 0
        finally:
            out, self.capture = bytes(self.capture), None
            self.noprompt = False
        over = len(out) > EXEC_MAX
        out = out[:EXEC_MAX]
        for i in range(0, len(out), CHUNK):
            self.frame(b"OUT " + base64.b64encode(out[i:i + CHUNK]))
        if self.sudo_blocked:
            self.sudo_blocked = False
            return self.err("sudo", "sudo needs a password: send AUTH, then the request again")
        self.frame("OK rc={} truncated={}".format(1 if rc else 0, 1 if over else 0))

    def l_bridge(self, rest):
        if rest not in ("", "on", "off"):
            return self.err("badreq", "BRIDGE on|off")
        if not self.netless:
            return self.err("nobridge", "this image has a network of its own")
        if rest:
            self.bridge_offered = rest == "on"
        self.frame("OK bridge={} chunk={}".format("on" if self.bridge_offered else "off", CHUNK))

    def l_auth(self, rest):
        try:
            pw = base64.b64decode(rest.encode("ascii"), validate=True).decode("utf-8") if rest else None
        except (binascii.Error, ValueError, UnicodeError):
            pw = None
        if pw is None:
            return self.err("badreq", "AUTH <password in base64>")
        self.auths.append(pw)
        if self.user not in self.admins:
            return self.err("perm", "{} is not an admin".format(self.user))
        if pw != self.users.get(self.user):
            return self.err("auth", "wrong password")
        self.grace = True
        self.frame("OK grace=300")

    def l_put(self, rest):
        parts = rest.split(" ", 2)
        if len(parts) != 3 or not parts[0].isdigit() or len(parts[1]) != 64:
            return self.err("badreq", "PUT <size> <sha256> <path>")
        try:
            binascii.unhexlify(parts[1])
        except (binascii.Error, ValueError):
            return self.err("badreq", "PUT <size> <sha256> <path>")
        size, want = int(parts[0]), parts[1].lower()
        # The file requests run with sudo while the grace holds (link.cpp's LinkSudo).
        was = self.elevated
        if self.grace and self.user in self.admins:
            self.elevated = True
        try:
            p = self._path(parts[2], True)
        finally:
            self.elevated = was
        if p is None:
            return
        if self.fs.isdir(p):
            return self.err("nofile", "{} is a directory".format(p))
        if size > self.fs.free() + len(self.fs.files.get(p, b"")):
            return self.err("nospace", "{} bytes free".format(self.fs.free()))
        if not self.fs.write(p, b""):
            return self.err("io", "could not open {} for writing".format(p))
        self.frame("READY chunk={}".format(CHUNK))
        got = bytearray()
        fail = None
        while len(got) < size:
            r = self.link_read(self.xfer_timeout)
            if r[0] == "timeout":
                fail = ("timeout", "nothing from the host for 10 s")
                break
            if r[0] == "intr" or (r[0] == "ok" and r[1] == "CANCEL"):
                self.cancels += 1
                fail = ("cancelled", "cancelled by the host")
                break
            line = r[1] if r[0] == "ok" else ""
            if len(line) < 3 or not line.startswith("D "):
                fail = ("badreq", "expected a D line")
                break
            try:
                data = base64.b64decode(line[2:].encode("ascii"), validate=True)
            except (binascii.Error, ValueError, UnicodeEncodeError):
                data = b""
            if not data or len(got) + len(data) > size:
                fail = ("badreq", "bad or oversized D line")
                break
            got.extend(data)
            self.fs.files[p] = bytes(got)
            self.frame("ACK {}".format(len(got)))
        if fail:
            self.fs.remove(p)
            return self.frame("ERR {} {}".format(*fail))
        if self.corrupt_put and got:
            got[0] ^= 0x01
        hexd = hashlib.sha256(bytes(got)).hexdigest()
        if hexd != want:
            self.fs.remove(p)
            return self.err("digest", "received {}; nothing was kept".format(hexd))
        self.fs.files[p] = bytes(got)
        self.frame("OK sha=" + hexd)

    def l_get(self, rest):
        was = self.elevated
        if self.grace and self.user in self.admins:
            self.elevated = True
        try:
            p = self._path(rest, False)
        finally:
            self.elevated = was
        if p is None:
            return
        if not self.fs.exists(p):
            return self.err("nofile", "there is no " + p)
        if self.fs.isdir(p):
            return self.err("nofile", "{} is a directory".format(p))
        data = self.fs.files[p]
        self.frame("SIZE {}".format(len(data)))
        fail = None
        for i, off in enumerate(range(0, len(data), CHUNK)):
            piece = data[off:off + CHUNK]
            if i == 1 and self.stall_get:
                time.sleep(self.stall_get)
            sent = piece
            if self.corrupt_get and i == 0:
                sent = bytes([piece[0] ^ 0x01]) + piece[1:]
            self.frame(b"D " + base64.b64encode(sent))
            r = self.link_read(self.xfer_timeout)
            if r[0] == "timeout":
                fail = ("timeout", "nothing from the host for 10 s")
                break
            if r[0] == "intr" or (r[0] == "ok" and r[1] == "CANCEL"):
                self.cancels += 1
                fail = ("cancelled", "cancelled by the host")
                break
            if r[0] != "ok" or not r[1].startswith("A"):
                fail = ("badreq", "expected A")
                break
        if fail:
            return self.frame("ERR {} {}".format(*fail))
        self.frame("OK sha=" + hashlib.sha256(data).hexdigest())

    def l_ls(self, rest):
        p = self._path(rest or ".", False)
        if p is None:
            return
        if not self.fs.exists(p):
            return self.err("nofile", "there is no " + p)
        if not self.fs.isdir(p):
            return self.err("nofile", "{} is not a directory".format(p))
        ents = self.fs.listdir(p)
        for name, d, size in ents:
            self.frame("E {} {} {}".format("d" if d else "f", 0 if d else size, name))
        self.frame("OK n={}".format(len(ents)))

    def l_stat(self, rest):
        p = self._path(rest, False)
        if p is None:
            return
        if not self.fs.exists(p):
            return self.err("nofile", "there is no " + p)
        if self.fs.isdir(p):
            self.frame("OK type=d size=0")
        else:
            self.frame("OK type=f size={}".format(len(self.fs.files[p])))


class PtyMock(object):
    """The mock on a pseudo-terminal: the device holds the master side and a
    real serial library opens `self.path`. POSIX only."""

    def __init__(self, dev):
        import tty
        self.dev = dev
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.stop = False
        self._out = threading.Thread(target=self._pump_out)
        self._in = threading.Thread(target=self._pump_in)
        for t in (self._out, self._in):
            t.daemon = True
        dev.host_opened(self)

    def start(self):
        self._out.start()
        self._in.start()
        return self

    def _pump_out(self):
        while not self.stop:
            try:
                data = self.dev.to_host.read(4096, 0.05)
            except Closed:
                return
            if data:
                try:
                    os.write(self.master, data)
                except OSError:
                    return

    def _pump_in(self):
        import select
        while not self.stop:
            try:
                r, _, _ = select.select([self.master], [], [], 0.05)
                if r:
                    data = os.read(self.master, 4096)
                    if data:
                        self.dev.from_host.write(data)
            except OSError:
                return

    def close(self):
        self.stop = True
        for t in (self._out, self._in):
            if t.is_alive():
                t.join(1.0)
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass
