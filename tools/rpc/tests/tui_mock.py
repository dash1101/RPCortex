"""The mock device with what the TUI reads, in the firmware's own formats
(os/shell/sys.cpp, ps.cpp, pkg.cpp, net.cpp, update.cpp, logcmd.cpp), and an
unplug that a test can pull and put back."""
import time

from mockdev import C_CYAN, C_GRAY, C_RESET, MockDevice, Reboot  # noqa: F401
from rpctool import transport
from rpctool.errors import RpcError


class TuiMock(MockDevice):
    def __init__(self, os_version="v2.0.640-beta", build="640", **kw):
        MockDevice.__init__(self, **kw)
        self.os_version = os_version
        self.build = build
        self.unplugged = False
        self.opens = 0
        self.reboots = []          # "reboot" or "sreboot", in order
        self.updates = []          # the argument lists `update` was given
        self.killed = []
        self.radio = "up"
        self.update_to = None      # what an update installs
        self.fs.files["/os/registry.bak"] = self.fs.files["/os/registry.cfg"]
        self.fs.files["/os/users.bak"] = self.fs.files["/os/users.cfg"]
        self.fs.dirs.add("/os/ssh")
        self.fs.files["/os/ssh/host_ed25519"] = b"\x02" * 32

    # --- the host's side ---------------------------------------------------------

    def opener(self, worker=None):
        """What the TUI's worker calls to open the port."""
        if self.unplugged or self.dead:
            raise RpcError("no RPCortex device found; plug one in, or start with --port")
        self.opens += 1
        return transport.Transport(self.open(), "mock")

    def unplug(self):
        """The cable pulled: the open handle fails from now on, and nothing can
        open the port until replug()."""
        self.unplugged = True
        self.gen += 1

    def replug(self):
        self.unplugged = False

    # --- link mode ------------------------------------------------------------------

    def _with_grace(self, fn, rest):
        # link.cpp runs LS and STAT under LinkSudo too, as it does PUT and GET:
        # with a grace from AUTH they may look at /os/ssh and the account file.
        was = self.elevated
        if self.grace and self.user in self.admins:
            self.elevated = True
        try:
            return fn(self, rest)
        finally:
            self.elevated = was

    def l_ls(self, rest):
        return self._with_grace(MockDevice.l_ls, rest)

    def l_stat(self, rest):
        return self._with_grace(MockDevice.l_stat, rest)

    # --- what the firmware prints ------------------------------------------------

    def hello(self):
        cwd = self.cwd.replace("%", "%25").replace(" ", "%20")
        arch = "armv6m" if self.board in ("pico", "pico_w") else "armv8m"
        self.frame("HELLO proto={} os={} build={} board={} arch={} user={} cwd={} chunk=768 "
                   "exec_max=32768".format(self.proto, self.os_version, self.build, self.board,
                                           arch, self.user, cwd))

    def c_ver(self, a):
        chip = "armv8m" if self.board.startswith("pico2") else "armv6m"
        self.out("RPCortex {}  (Vela II)\nCommit: 299568f  (beta)  [C++ native]\n"
                 "GCC 14.2.1   Platform: {} ({})\nFeatures: wifi, bluetooth, ssh, sd, "
                 "peripherals\n".format(self.os_version, self.board, chip))

    def c_sysinfo(self, a):
        self.out_info("=== RPCortex Vela II - System Info ===")
        self.out("  OS Version  : {}\n  Codename    : Vela II\n  Device ID   : vela\n"
                 "  Active User : {}\n  Platform    : {}\n".format(
                     self.os_version, self.user, self.board))

    def c_uptime(self, a):
        self.out("Uptime: 1h 2m 3s\n")

    def c_meminfo(self, a):
        self.out("  Total : 480 KB\n  Used  : 200 KB  (41%)\n  Free  : 280 KB\n"
                 "  Largest block : 180 KB\n\n  Reserved before the heap : 96 KB\n")

    def c__pkgs(self, a):
        self.out("PKGS_BEGIN\nOS:{}:{}\n".format(self.os_version, self.board))
        for k in sorted(self.installed):
            self.out("PKG:{}:{}\n".format(k, self.installed[k]))
        self.out("PKGS_END\n")

    def c_wifi(self, a):
        if self.user not in self.admins:
            self.out_err("'wifi' needs an admin account.")
            return 1
        if self.radio == "off":
            self.out("  Radio  : {}off{}\n  Use 'wifi scan' or 'wifi connect <ssid>' to bring "
                     "it up.\n".format(C_GRAY, C_RESET))
            return 0
        self.out("  Radio  : on\n  Link   : {}up{}\n  Network: home\n  Signal : {}####{}  "
                 "-55 dBm  (good)\n  Address: 192.168.1.40\n  Gateway: 192.168.1.1\n"
                 "  Netmask: 255.255.255.0\n".format(C_CYAN, C_RESET, C_CYAN, C_RESET))

    def c_ps(self, a):
        self.out("  PID  NAME         STATE  CORE STACK       CPU      \n"
                 "  {g}------------------------------------------------------------{r}\n"
                 "  {c}1   {r} init         {r}sleep {r} 0    -           12ms     \n"
                 "  {c}2   {r} shell        {c}run   {r} 0    2.8K/8.0K   1.2s     \n"
                 "  {c}7   {r} blinker      {r}sleep {r} 1    0.4K/3.0K   40ms     \n"
                 "  {g}3 tasks  ·  CPU 3%  ·  280 KB free{r}\n"
                 .format(c=C_CYAN, r=C_RESET, g=C_GRAY))

    def c_kill(self, a):
        if len(a) < 2 or not a[1].isdigit():
            self.out("Usage: kill <pid> [pid...]\n")
            return 1
        pid = int(a[1])
        if pid == 2:
            self.out_err("Cannot stop pid 2.  It is the shell.")
            return 1
        self.killed.append(pid)
        self.out_ok("Asked 'blinker' (pid {}) to stop.".format(pid))

    def c_logdump(self, a):
        self.out("     0.412  [:] boot ok\n     1.020  [?] [svc] slow start\n"
                 "     2.500  [!] [wifi] join failed\n  {}3 lines{}\n".format(C_GRAY, C_RESET))

    def c_reboot(self, a):
        if self.user not in self.admins:
            self.out_err("'reboot' needs an admin account.")
            return 1
        self.reboots.append("reboot")
        self.out_info("Rebooting system...")
        raise Reboot()

    def c_sreboot(self, a):
        if self.user not in self.admins:
            self.out_err("'sreboot' needs an admin account.")
            return 1
        self.reboots.append("sreboot")
        self.out_info("Performing soft reboot...")
        raise Reboot()

    def c_update(self, a):
        sub = a[1] if len(a) > 1 else "check"
        if sub not in ("online", "install"):
            self.out("Usage: update check|install\n")
            return 1
        if self.proto >= 2 and not self.elevated:
            self.out_err("'update {}' changes the system. Run it with sudo: sudo {}".format(
                sub, " ".join(a)))
            return 1
        self.updates.append(a[2:])
        force = "--force" in a or "-f" in a
        if "--yes" not in a and "-y" not in a:
            # session_confirm: nobody can answer in link mode, so this would
            # wait for keys that never come.
            self.out("  Continue? [y/N] ")
            while True:
                c = self.getkey(None)
                if c == 3:
                    return 1
        old = self.os_version.count(".") == 1          # vMAJOR.ITER
        if old and not force:
            self.out_ok("Already running {}. 'sudo update install --force' to reinstall."
                        .format(self.os_version))
            return 0
        self.out_ok("Checksum verified.")
        if self.update_to:
            self.os_version, self.build = self.update_to
        self.reboots.append("update")
        raise Reboot()


def wait_for(cond, timeout=8.0, step=0.02, pump=None):
    """Poll until cond() is true (pumping the app's events as it goes)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pump:
            pump()
        if cond():
            return True
        time.sleep(step)
    if pump:
        pump()
    return cond()
