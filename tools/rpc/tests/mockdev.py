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
  * firmware older than link mode, where `link` is not a command.

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
                 online=False, repaint=True, capacity=1024 * 1024):
        self.link_supported = link
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
        b = self.from_host.read(1, timeout)
        return b[0] if b else None

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
        fn = getattr(self, "c_" + name, None)
        if name == "link" and not self.link_supported:
            fn = None
        if fn is None:
            self.out_err("'{}' is not a command or executable file.".format(name))
            return 1
        return fn(args) or 0

    # --- permissions ------------------------------------------------------

    def may_read(self, p):
        if self.user == "root":
            return True
        return not (p in ("/os/users.cfg", "/os/device.key") or p.startswith("/home/root"))

    def may_change(self, p):
        if self.user == "root":
            return True
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

    def c_wget(self, a):
        if len(a) < 2:
            self.out("Usage: wget <url> [file]\n")
            return 1
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
        self.frame("HELLO proto=1 os=v2.01-beta build=554 board={} user={} cwd={} chunk={} "
                   "exec_max={}".format(self.board, self.user, cwd, CHUNK, EXEC_MAX))

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
        if not (self.may_change(p) if change else self.may_read(p)):
            self.err("perm", "{} is protected for this account".format(p))
            return None
        return p

    def l_exec(self, line):
        if not line:
            return self.err("badreq", "EXEC needs a command")
        self.capture = bytearray()
        try:
            rc = self.run_line(line)
        except Logout:
            rc = 0
        finally:
            out, self.capture = bytes(self.capture), None
        over = len(out) > EXEC_MAX
        out = out[:EXEC_MAX]
        for i in range(0, len(out), CHUNK):
            self.frame(b"OUT " + base64.b64encode(out[i:i + CHUNK]))
        self.frame("OK rc={} truncated={}".format(1 if rc else 0, 1 if over else 0))

    def l_put(self, rest):
        parts = rest.split(" ", 2)
        if len(parts) != 3 or not parts[0].isdigit() or len(parts[1]) != 64:
            return self.err("badreq", "PUT <size> <sha256> <path>")
        try:
            binascii.unhexlify(parts[1])
        except (binascii.Error, ValueError):
            return self.err("badreq", "PUT <size> <sha256> <path>")
        size, want = int(parts[0]), parts[1].lower()
        p = self._path(parts[2], True)
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
        p = self._path(rest, False)
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
