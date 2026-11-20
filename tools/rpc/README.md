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
| `wget` | `rpc wget https://example.com/data.csv` has the device download it itself; it must be online. |
| `backup` | `rpc backup device.zip` copies `/home`, `/etc`, `/usb` and everything else a person put on the device into a zip (or a folder). `/os`, `/tmp` and `/sd` are left out; `--with-packages` adds the installed packages. |
| `restore` | `rpc restore device.zip` puts it back and reinstalls any packages it holds. `--dry-run` says what it would do. |
| `reboot` | `rpc reboot --wait` restarts the device and waits for the port to return. |
| `log` | `rpc log` prints the device's log (`logdump`). |
| `ps` | `rpc ps` lists its tasks. |
| `flash` | `rpc flash out/rpcortex-v2-pico2_w.uf2` resets the board into BOOTSEL (by opening the port at 1200 baud), waits for its drive, checks the image is for that chip, and copies it on. The firmware is replaced and the files are kept. |
| `web` | `rpc web` serves the browser tool on http://localhost:8765 and opens it. `--port` picks another port. |
| `watch` | `rpc watch "meminfo" --every 5` runs a command again and again until Ctrl+C. |

Transfers show a progress bar when stderr is a terminal. Every `put` and `get`
is checked end to end: the device compares the SHA-256 of what it wrote, and
`rpc` compares the SHA-256 of what it received.

Paths with spaces or non-ASCII characters work as they are; quote them for
your own shell. `rm` refuses a name containing `*`, which the device would
treat as a wildcard.

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
`os/host/run_all.sh` runs them too.
