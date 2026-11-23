"""The one thread that talks to the device.

The link protocol is one request at a time, read to its final frame before
the next is sent (LINK.md), so exactly one thread owns the port: this one.
The screen never touches it. It hands the worker jobs, and the worker hands
back events (results, progress, a question it needs answered, what the
terminal printed), which the screen reads from a queue between keys.

Cancelling never writes from another thread either. The transport the Device
reads through is wrapped, and its read() raises Cancelled once when asked to.
The library's own recovery then runs on this thread: LinkClient sends CANCEL
for a transfer or Ctrl+C for a command, reads to the final frame and checks
the link with HELLO, exactly as it does after a Ctrl+C in `rpc`.
"""
import os
import queue
import threading
import time
import traceback

from rpctool.device import Device
from rpctool.errors import DeviceGone, LoginFailed, LoginRequired, RpcError
from rpctool.link import Timing


class Cancelled(RpcError):
    """The person pressed Esc during a job."""


class ChoosePort(RpcError):
    """Several devices are plugged in and none was named."""

    def __init__(self, ports):
        RpcError.__init__(self, "Several devices are connected; choose one.")
        self.ports = ports


class CancelTransport(object):
    """A Transport whose read() raises Cancelled once when `take()` says a
    cancel is pending. take() clears it as it answers, so the library's own
    clean-up after the exception (CANCEL, reading to the final frame, HELLO)
    reads through here undisturbed. Reads in short slices so a cancel is seen
    within 0.1 s."""

    SLICE = 0.1

    def __init__(self, inner, take=None):
        self.inner = inner
        self.take = take or (lambda: False)

    @property
    def name(self):
        return self.inner.name

    @property
    def closed(self):
        return getattr(self.inner, "closed", False)

    def _check(self):
        if self.take():
            raise Cancelled("cancelled")

    def read(self, timeout):
        end = time.monotonic() + max(0.0, timeout)
        while True:
            self._check()
            left = end - time.monotonic()
            data = self.inner.read(max(0.0, min(left, self.SLICE)))
            if data:
                return data
            if time.monotonic() >= end:
                return b""

    def write(self, data):
        self.inner.write(data)

    def discard_input(self):
        self.inner.discard_input()

    def close(self):
        self.inner.close()


class Job(object):
    """One piece of work for the worker. `fn(dev, job)` runs on the worker;
    `ok(result)` and `fail(exc)` run on the screen's thread."""

    _next = [0]

    def __init__(self, label, fn, ok=None, fail=None, cancellable=False, quiet=False,
                 expect_gone=False, link=True):
        self._next[0] += 1
        self.id = self._next[0]
        self.label = label
        self.fn = fn
        self.ok = ok
        self.fail = fail
        self.cancellable = cancellable
        self.quiet = quiet              # a periodic refresh: not shown as busy
        self.expect_gone = expect_gone  # the device going away is success (reboot, update)
        self.link = link                # needs a connected device
        self.worker = None
        self.cancel_requested = False

    def take_cancel(self):
        """True once, when a cancel is pending; whoever asks first acts on it."""
        if self.cancel_requested and self.cancellable:
            self.cancel_requested = False
            return True
        return False

    def progress(self, done, total, label=None):
        if self.take_cancel():
            # Raised from inside the library's own loop, which then cancels
            # the transfer on the wire (CANCEL) before passing it on.
            raise Cancelled("cancelled")
        self.worker._progress(self, label or self.label, done, total)

    def step(self, text):
        self.worker._post("step", self, text)

    def cancelled(self):
        return self.cancel_requested

    def ask(self, kind, text, masked=False, default=""):
        """Ask the person something and wait (on the worker) for the answer;
        None if they cancelled."""
        return self.worker._ask(kind, text, masked, default)


class Worker(threading.Thread):
    """Owns the Device. Connects, reconnects after an unplug or a restart,
    runs jobs, sends a PING now and then so an unplugged board is noticed,
    and in terminal mode passes bytes both ways."""

    HEARTBEAT = 5.0
    RETRY = 2.0
    AFTER_FLASH = 2.0       # before looking for the console again

    def __init__(self, opener, events=None, user=None, timing=None, env_password=None,
                 port=None):
        threading.Thread.__init__(self, name="rpc-link")
        self.daemon = True
        self.opener = opener                 # opener(worker) -> Transport
        self.events = events or queue.Queue()
        self.timing = timing or Timing()
        self.user = user
        self.port = port                     # named by the person (--port, or chosen)
        self.last_port = None                # the one connected last
        self._login_pw = None
        self._env_pw = env_password if env_password is not None else os.environ.get("RPC_PASSWORD")
        self._env_tried = False
        self.jobs = queue.Queue()
        self.keys = queue.Queue()
        self.dev = None
        self.tx = None
        self.state = "connecting"
        self.current = None
        self.terminal = False
        self._want_term = None               # True: enter, False: leave
        self._stop = threading.Event()
        self._hold = False                   # do not reconnect (flashing, waiting for an answer)
        self._next_try = 0.0
        self._last_io = 0.0
        self._last_progress = 0.0

    # --- called from the screen's thread --------------------------------------

    def submit(self, job):
        job.worker = self
        self.jobs.put(job)
        return job

    def idle(self):
        return self.current is None and self.jobs.empty()

    def busy_job(self):
        c = self.current
        return c if c is not None and not c.quiet else None

    def cancel(self):
        """Cancel the job in hand, if it can be. True when asked."""
        c = self.current
        if c is None or not c.cancellable:
            return False
        c.cancel_requested = True
        return True

    def _take_cancel(self):
        c = self.current
        return c.take_cancel() if c is not None else False

    def enter_terminal(self):
        self._want_term = True

    def leave_terminal(self):
        self._want_term = False

    def send_keys(self, data):
        self.keys.put(bytes(data))

    def set_login(self, user, password):
        self.user = user
        self._login_pw = password
        self._hold = False
        self._next_try = 0.0

    def set_port(self, path):
        self.port = path
        self._hold = False
        self._next_try = 0.0

    def retry_now(self):
        self._hold = False
        self._next_try = 0.0

    def stop(self):
        self._stop.set()

    # --- events ---------------------------------------------------------------

    def _post(self, *ev):
        self.events.put(ev)

    def _set_state(self, state, detail=""):
        self.state = state
        self._post("state", state, detail)

    def _progress(self, job, label, done, total):
        now = time.monotonic()
        if done < total and now - self._last_progress < 0.1:
            return
        self._last_progress = now
        self._post("progress", job, label, done, total)

    def _ask(self, kind, text, masked=False, default=""):
        box = queue.Queue(1)
        self._post("ask", kind, text, masked, default, box.put)
        while not self._stop.is_set():
            try:
                return box.get(timeout=0.2)
            except queue.Empty:
                continue
        return None

    def _noise(self, line):
        if line.strip():
            self._post("noise", line)

    # --- passwords -------------------------------------------------------------

    def _password(self):
        if self._login_pw:
            return self._login_pw
        return self._ask("login-password", "Password for {} on the device".format(self.user),
                         masked=True)

    def _sudo_password(self):
        # $RPC_PASSWORD first, once per connection; then the person is asked,
        # in the TUI's own masked prompt. The answer goes straight to the
        # Device, which sends it with AUTH and keeps it for the connection,
        # as `rpc` does. Nothing here stores it or prints it.
        if self._env_pw and not self._env_tried:
            self._env_tried = True
            return self._env_pw
        who = self.dev.hello.get("user", "this account") if self.dev else "this account"
        return self._ask("sudo", "sudo: password for {} on the device".format(who), masked=True)

    # --- connecting --------------------------------------------------------------

    def _connect(self):
        self._set_state("connecting")
        try:
            inner = self.opener(self)
        except ChoosePort as e:
            self._hold = True
            self._set_state("choose-port", str(e))
            self._post("ports", e.ports)
            return
        except RpcError as e:
            self._set_state("disconnected", str(e))
            self._next_try = time.monotonic() + self.RETRY
            return
        tx = CancelTransport(inner, self._take_cancel)
        dev = Device(tx, user=self.user, password=self._password, timing=self.timing,
                     on_noise=self._noise, sudo_password=self._sudo_password)
        self._env_tried = False
        try:
            dev.connect()
        except (LoginRequired, LoginFailed) as e:
            dev.close()
            self._hold = True
            self._set_state("login", str(e))
            self._post("login", str(e), self.user)
            return
        except RpcError as e:
            dev.close()
            self._set_state("disconnected", str(e))
            self._next_try = time.monotonic() + self.RETRY
            return
        self.dev, self.tx = dev, tx
        # Remembered, not fixed: after a restart the port can come back under
        # another name, and the opener looks again.
        self.last_port = inner.name
        self._last_io = time.monotonic()
        self._set_state("connected", inner.name)
        self._post("hello", dict(dev.hello), dev.mode)

    def _drop(self, reason, delay=1.0):
        """The device went away: let go of the port and try again shortly."""
        dev, self.dev, self.tx = self.dev, None, None
        self.terminal = False
        if dev is not None:
            dev.gone = True
            try:
                dev.close()
            except Exception:
                pass
        self._set_state("disconnected", reason)
        self._next_try = time.monotonic() + delay

    def release_port(self):
        """For a job that needs the port itself (BOOTSEL flash): leave link
        mode, close the port, and do not reconnect until the job is over.
        Returns the port's path. Worker thread only."""
        dev = self.dev
        path = dev.t.name if dev is not None else (self.port or self.last_port)
        self._hold = True
        self.dev, self.tx = None, None
        if dev is not None:
            try:
                dev.close()
            except Exception:
                pass
        self._set_state("flashing", path or "")
        return path

    # --- the loop ---------------------------------------------------------------

    def run(self):
        try:
            while not self._stop.is_set():
                self._tick()
        finally:
            if self.dev is not None:
                try:
                    self.dev.close()
                except Exception:
                    pass
            self._post("stopped")

    def _tick(self):
        if self.dev is None:
            self._fail_queued("The device is not connected.")
            if not self._hold and time.monotonic() >= self._next_try:
                self._connect()
            else:
                self._run_unlinked(0.1)
            return
        if self._want_term is not None:
            want, self._want_term = self._want_term, None
            self._switch_terminal(want)
            return
        if self.terminal:
            self._pump_terminal()
            return
        try:
            job = self.jobs.get(timeout=0.1)
        except queue.Empty:
            job = None
        if job is not None:
            self._run(job)
            return
        if time.monotonic() - self._last_io >= self.HEARTBEAT and self.dev.mode == "link":
            try:
                self.dev.link.ping()
                self._last_io = time.monotonic()
            except DeviceGone as e:
                self._drop("the device went away ({})".format(e))
            except RpcError as e:
                self._recover(str(e))

    def _fail_queued(self, why):
        """Jobs that need the device fail at once while it is away; ones that
        do not (a flash with the board already in BOOTSEL) still run."""
        keep = []
        while True:
            try:
                job = self.jobs.get_nowait()
            except queue.Empty:
                break
            if job.link:
                self._post("fail", job, RpcError(why))
            else:
                keep.append(job)
        for j in keep:
            self.jobs.put(j)

    def _run_unlinked(self, wait):
        try:
            job = self.jobs.get(timeout=wait)
        except queue.Empty:
            return
        if job.link:
            self._post("fail", job, RpcError("The device is not connected."))
        else:
            self._run(job)

    def _run(self, job):
        job.cancel_requested = False
        self.current = job
        self._post("start", job)
        try:
            result = job.fn(self.dev, job)
        except Cancelled as e:
            self._post("fail", job, e)
        except DeviceGone as e:
            if job.expect_gone:
                self._post("done", job, None)
            else:
                self._post("fail", job, e)
            self._drop("the device went away ({})".format(e), delay=1.5)
        except RpcError as e:
            self._post("fail", job, e)
        except Exception as e:  # keep the screen alive whatever a job does
            self._post("fail", job, RpcError("{}: {}\n{}".format(
                type(e).__name__, e, traceback.format_exc(limit=3))))
        else:
            self._post("done", job, result)
        finally:
            self.current = None
            self._last_io = time.monotonic()
        if self.dev is not None and self.dev.gone:
            self._drop("the device is restarting", delay=1.5)
        elif self._hold and self.dev is None and self.state == "flashing":
            self._hold = False
            self._next_try = time.monotonic() + self.AFTER_FLASH
            self._set_state("disconnected", "waiting for the board to come back")
        elif self.dev is not None and self.dev.mode == "link" and self.dev.link.dropped:
            self._recover("link mode ended")

    def _recover(self, why):
        """Link mode ended under us (someone pressed Ctrl+C on the console, a
        timeout): get back into it on the same port, or start again."""
        try:
            self.dev.connect()
            self._post("hello", dict(self.dev.hello), self.dev.mode)
            self._set_state("connected", self.dev.t.name)
        except (LoginRequired, LoginFailed) as e:
            self._drop(str(e))
            self._hold = True
            self._set_state("login", str(e))
            self._post("login", str(e), self.user)
        except RpcError as e:
            self._drop("{}; reconnecting ({})".format(why, e))

    # --- terminal ----------------------------------------------------------------

    def _switch_terminal(self, want):
        dev = self.dev
        if want and not self.terminal:
            # Out of link mode (BYE) with the port kept open; the prompt comes
            # back and from then on bytes pass through untouched.
            if dev.mode == "link" and not dev.link.dropped:
                dev.link.bye()
            if dev.mode == "link" and dev.link is not None:
                left = bytes(dev.link.r.buf)
                del dev.link.r.buf[:]
                # What followed the final frame (the BYE frame, "Left link
                # mode.", the prompt): the frames are not for a person.
                left = b"\n".join(ln for ln in left.split(b"\n") if b"\x1e" not in ln)
                if left:
                    self._post("term", left)
            self.terminal = True
            self._drain_keys()
            self._set_state("terminal", dev.t.name)
        elif not want and self.terminal:
            self.terminal = False
            self._drain_keys()
            # Device.connect() gets from whatever the console shows (a prompt,
            # a command still running, a login prompt) back into link mode.
            self._recover("left the terminal")

    def _drain_keys(self):
        while True:
            try:
                self.keys.get_nowait()
            except queue.Empty:
                return

    def _pump_terminal(self):
        t = self.dev.t
        try:
            while True:
                try:
                    data = self.keys.get_nowait()
                except queue.Empty:
                    break
                t.write(data)
            data = t.read(0.05)
            if data:
                self._post("term", data)
        except DeviceGone as e:
            self._drop("the device went away ({})".format(e))
