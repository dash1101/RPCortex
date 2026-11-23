# rpc: the command-line tool

`rpc` drives an RPCortex v2 device from a computer over its USB serial
console: run commands and get their exit status, move files both ways with a
SHA-256 check, install packages, back the device up, flash new firmware. It
speaks the link protocol ([LINK.md](../../LINK.md)) and falls back to the
prompt on firmware older than that. Linux, macOS and Windows; Python 3.8 or
later; one dependency, pyserial.

## Installing

From a checkout of this repository:

```
pip install -e tools/rpc
```

That puts `rpc` on the PATH. The `-e` keeps it pointing at the checkout, which
is also where `rpc web` finds the browser tool. Without installing anything:

```
pip install pyserial
python3 tools/rpc/rpc.py info          # or: cd tools/rpc && python3 -m rpctool info
```

## Finding the device

With one board plugged in there is nothing to configure: `rpc` looks for a USB
serial port from Raspberry Pi (vendor 0x2E8A) that names itself RPCortex. With
several it lists them and asks. To choose one yourself, use `--port` or set
`RPC_PORT`:

```
rpc --port /dev/ttyACM0 info           # Linux
rpc --port /dev/cu.usbmodem1101 info   # macOS
rpc --port COM5 info                   # Windows
export RPC_PORT=/dev/ttyACM0
```

When the device is at a login prompt, `--user NAME` logs in; the password is
asked for, or read from `RPC_PASSWORD`. Without `--user` nothing is typed at the
prompt and `rpc` says what it found.

## sudo

On firmware with link protocol 2, installing a package, changing anything in
`/os` or `/etc` (`put`, `rm`, `mkdir`, `mv` there, and `restore`) and reading the
account file or the keys (`get`, `cat`) need sudo, which
asks for the signed-in account's password (PERMISSIONS.md). `rpc` asks for it the
first time the device says so, sends it with `AUTH` (LINK.md) and repeats the
request; a typed password that is wrong is asked for again, three times at most.
`RPC_PASSWORD`, when it is set, is used instead of asking, and tried once.
`--sudo` gives it before anything else, so a long job is not stopped half way to
ask. `rpc exec` runs the line as written: `rpc exec "sudo pkg remove gps"`.
Firmware without sudo is never asked.

## The network bridge

A Pico or Pico 2 has no radio, and a Pico 2 W can be built without Wi-Fi. On those
boards (firmware with link protocol 3), `rpc` lends the device this computer's
connection while it is attached. On a Pico, in `rpc term`:

```
root@vela:/> sudo pkg install greet
Password for root, to run 'pkg install' ••>  ••••••
[:] No package list yet; fetching it first.
[:] Fetching package lists...
    ...
[:] Installing greet 1.0
[@] Checksum verified.
[@] Installed greet 1.0 (1436 bytes).
```

That just works; so do `pkg update`, `update check`, `sudo update install`, `wget`,
`curl` and `runurl`, at the prompt in `rpc term` or as `rpc exec "sudo pkg install
greet"`. The device asks, `rpc` fetches the URL with this computer's network,
checks HTTPS against this computer's trusted certificates, follows redirects, and
sends the body back over the cable one acknowledged chunk at a time. Each request
is logged on stderr (`[rpc] bridge: GET https://...`), with a progress bar outside
the terminal.

It is on by default, and `rpc` decides what is fetched:

- only `http` and `https`, on every redirect too; never a file on this computer;
- not this computer itself (`localhost`, `127.0.0.1`, `::1`) or a link-local
  address (`169.254.x.x`, where cloud machines keep their credentials), unless
  named with `--bridge-allow`; a package on the device has no business reaching
  the services on this computer. Other machines on the local network are
  fetched like any other host; `--bridge-allow` narrows that too;
- `--bridge-allow raw.githubusercontent.com` (repeat it, or a comma list;
  `.example.com` covers the subdomains) fetches only from those hosts;
- `--bridge-max 2M` caps one download (8 MB unless told);
- `--no-bridge` turns it off, and the device says it has no network.

In `rpc term` the bridge's traffic never reaches the screen, and what is typed
while a download runs is held and sent when it ends; Ctrl+C goes straight
through and stops it. Only commands run at the console use the bridge: a `bg`
job or a service on the device gets no network. A board with Wi-Fi of its own
never asks. LINK.md, "The network bridge", has the protocol.

Every command leaves the device at its normal prompt when it finishes,
including after an error or Ctrl+C. Lines the device's background services
print while `rpc` works are shown on stderr as `[device] ...`; `-q` hides them
and the progress bars.

## Commands

Each has `--help`. The exit status is 0 on success and 1 on failure; `exec`
returns the command's own status, 124 when `--timeout` ran out, and 130 after
Ctrl+C.

| Command | Example |
|---|---|
| `ports` | `rpc ports` lists RPCortex devices and any BOOTSEL drive. |
| `info` | `rpc info` shows the firmware, board, account, memory and storage. |
| `term` (or `shell`) | `rpc term` is an interactive terminal on the console. Ctrl+] quits. |
| `exec` | `rpc exec "ls /home ; df"` runs a line through the device's shell. `--timeout 10` sends Ctrl+C after ten seconds. |
| `ls` | `rpc ls /home/root` |
| `stat` | `rpc stat /os/registry.cfg` |
| `put` | `rpc put notes.txt /home/root/` sends a file; `rpc put -r site /home/root/site` sends a folder. The default destination is `/<name>`. |
| `get` | `rpc get /home/root/notes.txt`; `rpc get -r /home/root backup-of-root` |
| `rm` | `rpc rm /tmp/old.txt`; `rpc rm -r /home/root/scratch` |
| `mkdir` | `rpc mkdir -p /home/root/a/b` |
| `mv` | `rpc mv /home/root/a.txt /home/root/b.txt` |
| `cat` | `rpc cat /etc/startup.cfg` |
| `install` | `rpc install build/apps/greet.app` installs a package file. `rpc install calc` downloads `calc` from the package repository on this computer, checks it against the SHA-256 in the index, and installs it, so the device needs no network. `rpc install --list` shows what the repository offers; `--index` points at another one. |
| `fetch` | `rpc fetch https://example.com/data.csv /home/root/` downloads on this computer and sends the file. |
| `wget` | `rpc wget https://example.com/data.csv` has the device download it itself, over its own network or, on a board without one, this computer's (the bridge). |
| `backup` | `rpc backup device.zip` copies `/home`, `/etc`, `/usb` and everything else a person put on the device into a zip (or a folder). `/os`, `/tmp` and `/sd` are left out, and the summary says so. `--with-packages` adds the installed packages. `--with-system` adds the settings and saved Wi-Fi networks, the accounts, the device key and the SSH host key, read with sudo. Keep a backup like that as safe as the device. |
| `restore` | `rpc restore device.zip` puts it back and reinstalls any packages it holds. `--dry-run` says what it would do. System files are put back only after a question that names each one (`--yes` answers it, `--skip-system` leaves them), and then the device restarts without saving, since a plain `reboot` would write its settings back over them. |
| `reboot` | `rpc reboot --wait` restarts the device and waits for the port to return. |
| `log` | `rpc log` prints the device's log (`logdump`). |
| `ps` | `rpc ps` lists its tasks. |
| `flash` | `rpc flash out/rpcortex-v2-pico2_w.uf2` resets the board into BOOTSEL (by opening the port at 1200 baud), waits for its drive, checks the image is for that chip, and copies it on. The firmware is replaced and the files are kept. |
| `web` | `rpc web` serves the browser tool on http://localhost:8765 and opens it. `--port` picks another port. |
| `watch` | `rpc watch "meminfo" --every 5` runs a command again and again until Ctrl+C. |

| `tui` | `rpc tui` is a full-screen program for the device. See below. |

Transfers show a progress bar when stderr is a terminal. Every `put` and `get`
is checked end to end: the device compares the SHA-256 of what it wrote, and
`rpc` compares the SHA-256 of what it received.

Paths with spaces or non-ASCII characters work as they are; quote them for
your own shell. `rm` refuses a name containing `*`, which the device would
treat as a wildcard.

## The TUI

`rpc tui` (or `python -m rpctool.tui`, or `rpc-tui`) puts the whole device on one
screen. It is not a wrapper that types commands into a terminal. It talks to the
device over the link protocol through the same library as `rpc`, from one
background thread that owns the port, so the screen never waits on the device.
It takes `--port` and `--user` like `rpc` does, and `--version` prints its version.

It looks like Orbit: the device and its state along the top, the screens in a
"nav" box on the left, each with a status square, the screen itself on the
right, and a "status" box at the bottom with the latest message and the keys
for the screen in view.

| Screen | What it shows | Keys |
|---|---|---|
| 1 device | the version, board, build, features, account and port; memory, storage, uptime and Wi-Fi, read again every 5 seconds (Wi-Fi every 30) | `r` read again, `w` Wi-Fi now, `b` reboot, `f` flash a `.uf2` from this computer, `B` back up, `R` restore |
| 2 files | the device on the left, this computer's current folder on the right | `←` `→` pick a side, `Enter` open a folder or view a file, `Backspace` the folder above, `c` copy to the other side, `v` view, `m` new folder, `n` rename, `d` delete, `r` list again |
| 3 packages | what is installed beside what the package repository offers for this board, with `update` where the versions differ | `i` install, `u` upgrade, `U` upgrade everything, `d` remove, `a` install a `.app` from this computer, `r` read again |
| 4 firmware | the running version against the latest release | `u` update with `sudo update online`, `f` download the `.uf2` for this board and flash it through BOOTSEL, `r` check again |
| 5 logs + tasks | `ps` and the log, read every 3 seconds, and any lines the device prints by itself | `↑` `↓` pick a task, `x` stop it, `PgUp` `PgDn` scroll the log |
| 6 terminal | the device's own console | `Enter` opens it, every key then goes to the device, `Ctrl+]` comes back |

Everywhere: `1` to `6` or `Tab` switch screens, `Esc` cancels what is running
(a copy, a download, an update), `t` changes the theme, `?` shows every key, and
`q` quits. Copies are checked with SHA-256 both ways, and anything that deletes or
replaces asks first.

**sudo.** When the device says a request needs sudo, a masked prompt asks for the
signed-in account's password, sends it with `AUTH`, and repeats the request.
`$RPC_PASSWORD` is tried first when it is set. The password is never shown,
written down or logged.

**Terminal.** Opening it leaves link mode with `BYE` and keeps the port open, so
the prompt is right there. `Ctrl+]` goes back into link mode the way `rpc` always
does (a Ctrl+C, then `link`), so anything still running at the prompt is
interrupted.

**Unplugging.** With the cable pulled, the top bar says disconnected. Plugged back
in, the board is found again on its own, as it is after a reboot, an update or a
flash.

**Firmware and the version change.** Since 2026-11-21 versions read
`v2.0.<build>`. A device on v2.01-beta to v2.05-beta compares those numbers as
older than its own, so the Firmware screen says so, and its one update uses
`--force`. After that, updates are offered as usual.

**Backup.** `B` on the Device screen backs up to a folder or a `.zip`, with
switches for the packages and the system files, and says what it left out. `R`
restores; with system files in the backup it names each one and asks whether to
put back everything or only the files.

**Themes.** `t` cycles tui (the graphite default), phosphor, amber, ember, frost,
lilac, mono, ocean and paper, and the choice is remembered in `config.json` in
`~/.config/rpc-tui` (or `$XDG_CONFIG_HOME`) on Linux, `~/Library/Application
Support/rpc-tui` on macOS and `%APPDATA%\rpc-tui` on Windows. Colours are the
exact ones on a terminal with 24-bit colour, the nearest of 256 on most others,
and the basic eight where that is all there is. Without colour it still reads
well, in bold, dim and reverse. `--colours 256|8|mono` sets the depth by hand,
and `--ascii` draws the boxes with plain characters. It needs at least 80 by 24 and
follows the window when it is resized.

**Windows.** Python on Windows has no curses. `rpc tui` says so and gives the one
command that adds it: `pip install windows-curses`. It works in Windows Terminal
and the classic console.

## Older firmware

Firmware from before link mode answers `'link' is not a command`. `rpc` then
works through the prompt and says so: `exec` types the command (with a marker
after it to find the end of its output, and its exit status from the shell's
own `&&` and `||`), and `put` and `install` use the old `put` command. `get`,
`ls`, `stat`, `cat` of binary files and `backup` need link mode; `rpc flash`
updates the firmware.

## Notes per system

**Linux.** The port is `/dev/ttyACM0` or similar, owned by the `dialout` group
(`uucp` on Arch). Add yourself and log in again:
`sudo usermod -aG dialout $USER`. On distributions that run ModemManager, it
can probe a newly plugged-in serial port for a few seconds; if the first command
after plugging in gets a garbled answer, wait a moment and run it again. `rpc
flash` needs the BOOTSEL drive mounted, which desktop environments do on their
own; without a desktop, mount it by hand or install `picotool`, which `rpc
flash` uses when no drive appears.

**macOS.** The port is `/dev/cu.usbmodem…` (use the `cu.` one, not `tty.`). No
driver is needed. The BOOTSEL drive mounts under `/Volumes`. If HTTPS
downloads fail with a certificate error, run the "Install Certificates" command
that comes with python.org's Python.

**Windows.** The port is `COM3`, `COM4` and so on; Device Manager lists it under
"Ports (COM & LPT)" as a USB Serial Device. Windows 10 and 11 need no driver.
Only one program can have a COM port open, so close other terminals first.
`rpc term` works in Windows Terminal and the classic console. The BOOTSEL drive
appears as a new drive letter.

## As a library

```python
from rpctool import Device

with Device.open() as dev:              # or Device.open("/dev/ttyACM0", user="root", password="...")
    print(dev.hello["os"], dev.hello["board"])
    r = dev.exec("ls /home")
    print(r.rc, r.text)
    dev.put(b"hello\n", "/home/root/hi.txt")
    print(dev.get("/home/root/hi.txt"))
    for entry in dev.ls("/home/root"):
        print(entry.type, entry.size, entry.name)
```

Every error is an `rpctool.RpcError`; a refusal from the device is a
`LinkError` with the protocol's error `code` (`nofile`, `perm`, `digest`, …).
`Device(..., sudo_password=...)` takes the account's password (or a function that
asks for it) for sudo; `dev.sudo_line("pkg install x")` puts `sudo` in front where
the firmware has it, and `exec` and `put` send `AUTH` and try again when the
device asks for sudo.
`Device` takes a `Transport`, which wraps anything shaped like pyserial's
`Serial` (`read`, `write`, `in_waiting`, `close`), so a script can hand it
something other than a real port.

## Tests

```
python3 -m unittest discover tools/rpc/tests
```

No hardware and no serial port: `tests/mockdev.py` is a pretend device written
from the firmware's source (the prompt and its line editor, the login prompt,
link mode as `os/shell/link.cpp` implements it, the old `put`, firmware without
link mode), with CRLF line endings and background lines dropped between frames.
The tests run every command against it, the fallback for old firmware, logging
in, recovering a device an earlier client left in link mode or mid-transfer,
cancelling with Ctrl+C, digest errors in both directions, timeouts on both
sides, and paths with spaces and Unicode. With pyserial installed, one test
also runs the real serial library against the mock on a pseudo-terminal.
`tests/test_tui.py` drives the TUI key by key against the mock, drawing on a grid
of text in place of curses: every screen's actions, the sudo prompt, unplugging
and plugging back in, cancelling, the terminal and backups with system files.
`test_bridge.py` gives the mock no network (`netless=True`): it asks for URLs in
the middle of an `exec` and at its prompt under `rpc term`, and `rpc` answers
from an HTTP server on 127.0.0.1 (redirects, a 404, gzip, a slow server, the
allowlist on every hop, the size cap, a cancel from either side).
`os/host/run_all.sh` runs them too.
