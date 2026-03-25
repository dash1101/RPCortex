# RPCortex Nebula — Documentation
### v0.8.1

---

## Contents

1. [Getting Started](#getting-started)
2. [Boot Sequence](#boot-sequence)
3. [The Shell](#the-shell)
4. [Filesystem Commands](#filesystem-commands)
5. [System Commands](#system-commands)
6. [User Management](#user-management)
7. [Networking](#networking)
8. [Package Manager](#package-manager)
9. [Editor](#editor)
10. [Settings Panel](#settings-panel)
11. [Registry](#registry)
12. [POST](#post)
13. [Session Logging](#session-logging)
14. [Hardware & Performance](#hardware--performance)
15. [Supported Hardware](#supported-hardware)
16. [Security](#security)
17. [Known Limitations](#known-limitations)

---

## Getting Started

**What you need:**
- Raspberry Pi Pico, Pico W, Pico 2, or compatible ESP32 board
- MicroPython firmware v1.20 or newer (v1.27+ recommended)
- A serial terminal at **115200 baud** — PuTTY on Windows, minicom or screen on Linux/macOS

**Thonny note:** Thonny's built-in REPL works for basic use but arrow key navigation, history, and the editor all require a proper terminal emulator. Use PuTTY for the full experience.

### Installation

1. Flash MicroPython to your board
2. Copy all files from the repo to the root of the board's filesystem
3. Connect your serial terminal at 115200 baud
4. Reboot — `main.py` starts automatically on power-up

### First boot

A setup wizard runs on first boot:
1. You set the `root` password (the administrator account)
2. A `guest` account is created automatically — it accepts any password including blank
3. You're offered the option to add the official package repo

After that, log in and you're at the shell prompt. The wizard only runs once.

---

## Boot Sequence

```
main.py
  └── Core/post.py              POST — hardware checks, registry, WiFi
       └── Core/initialization.py   reads startup mode, runs login loop
            └── Core/launchpad.py   launchpad_init — the interactive shell
```

If initialization fails, `recovery_init()` starts an unauthenticated shell so you can diagnose the problem. The recovery shell has access to all the same commands.

---

## The Shell

The shell prompt format is:

```
username@nebula:~>
username@nebula:~/docs>
username@nebula:/Core>
```

`~` is shorthand for the user's home directory (`/Users/<username>/`). The shell starts there on login. `cd ~` and bare `cd` both return home.

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| Up / Down | Scroll command history (last 50) |
| Left / Right | Move cursor within the line |
| Home / Ctrl+A | Jump to beginning of line |
| End / Ctrl+E | Jump to end of line |
| Delete | Delete character under cursor |
| Backspace | Delete character before cursor |
| Ctrl+C | Cancel current input |

Characters are inserted at the cursor position — you can edit anywhere in the line, not just at the end.

### Command loading

Built-in commands (`sys_fs`, `sys_sys`, `sys_net`, `sys_user`, `wifi`, `settings`) are loaded via `__import__()` and live in `sys.modules` permanently. After a cache clear they reload for free — no re-reading or recompiling. Package-installed commands use a separate exec-based path.

### Critical commands

`reboot`, `sreboot`, `freeup`, and `gc` are implemented as inline functions that bypass the loader entirely. They always work regardless of heap state.

### Heap management

The RP2040 has 264KB of RAM. After loading several commands or running HTTPS requests, the heap may become fragmented — `gc.mem_free()` can show 90KB free while a `MemoryError` still occurs, because no single contiguous block is large enough for a new allocation. Run `freeup` to compact the heap. The shell also automatically retries after a cache clear on `MemoryError`.

---

## Filesystem Commands

| Command | Description |
|---------|-------------|
| `ls [path]` | List directory — type, size, modification time, name |
| `cd <path>` | Change directory. `cd`, `cd ~`, and `cd ~/sub` all work |
| `pwd` | Print working directory |
| `mkdir <path>` | Create directory |
| `rm <path>` | Remove file or directory (recursive for directories) |
| `touch <file>` | Create an empty file |
| `read <file>` | Print file contents |
| `head <file> [n]` | First n lines (default 10) |
| `tail <file> [n]` | Last n lines (default 10) |
| `mv <src> <dst>` | Move or rename |
| `cp <src> <dst>` | Copy file |
| `rename <old> <new>` | Rename in place |
| `df` | Flash usage — total, used, free |
| `tree [path]` | Recursive directory listing |
| `exec <file>` | Run a Python file directly |

---

## System Commands

| Command | Description |
|---------|-------------|
| `sysinfo` | OS version, user, CPU, RAM, flash summary |
| `meminfo` | Detailed RAM breakdown |
| `uptime` | Time since last boot |
| `date` | Current date/time from RTC |
| `ver` | OS version string |
| `clear` / `cls` | Clear the terminal |
| `fetch` / `neofetch` | System info display with ASCII logo |
| `reboot` | Hard reset |
| `sreboot` / `softreset` | Soft reset (faster, keeps filesystem) |
| `freeup` / `gc` | Clear command cache, run GC, report RAM freed |
| `echo <text>` | Print text |
| `history` | Show command history |
| `env [section]` | Dump registry contents |
| `reg get <key>` | Read a registry key |
| `reg set <key> <val>` | Write a registry key |
| `settings` | Open the settings panel |
| `edit [file]` | Open the built-in editor |
| `bench` | Run the NebulaMark benchmark |

### `pulse` subcommands

| Command | Description |
|---------|-------------|
| `pulse status` | Current CPU frequency and temperature |
| `pulse oc [MHz]` | Overclock to MHz |
| `pulse uc [MHz]` | Underclock to MHz |
| `pulse boot on` | Enable boot overclocking |
| `pulse boot off` | Disable boot overclocking |

---

## User Management

| Command | Description |
|---------|-------------|
| `whoami` | Current logged-in user |
| `mkacct` | Create a new account (prompts for username and password) |
| `rmuser <user>` | Delete an account. Non-root users must verify the target's password first. |
| `chpswd <user>` | Change a password |
| `logout` / `exit` | End session, return to login |

### Account details

- Profiles stored in `/Nebula/Registry/user.cfg` as CSV
- Each account gets `/Users/<username>/` created on account creation
- Passwords stored as **salted SHA256** — unique salt per account
- `guest` uses the `NOPASS` marker — any password, including blank, is accepted
- `root` is created during first-run setup and is the administrator account

---

## Networking

WiFi works on any board with a `network` module — Pico W, Pico 2 W, most ESP32 variants.

### WiFi commands

| Command | Description |
|---------|-------------|
| `wifi status` | Connection state, SSID, IP |
| `wifi scan` | Nearby networks with signal strength |
| `wifi connect [ssid]` | Connect (prompts for password; uses saved if available) |
| `wifi disconnect` | Drop the current connection |
| `wifi list` | Saved networks |
| `wifi add <ssid>` | Save a network |
| `wifi forget <ssid>` | Remove a saved network |

Up to 2 networks can be saved. Enable autoconnect on boot:

```
reg set Settings.Network_Autoconnect true
```

Or toggle it in `settings`.

### Download commands

| Command | Description |
|---------|-------------|
| `wget <url> [file]` | Download to flash — streams directly, no full-file RAM load |
| `curl <url> [-v]` | Fetch and print response body |
| `runurl <url> [--keep]` | Fetch and execute a Python file; deletes after unless `--keep` |
| `ping <host> [n]` | TCP connectivity test |
| `nslookup <host>` | DNS lookup |

Both HTTP and HTTPS are supported. The client follows redirects iteratively (no recursion) with a 15-second socket timeout.

**HTTPS note (Pico 1 W):** TLS needs ~9.5KB contiguous heap. Run `freeup` before network-heavy work on a Pico 1, or use HTTPS from a fresh boot. If your host uses Cloudflare's "Always Use HTTPS" redirect, HTTP requests will be 301'd and may hit this limit — disable it for your repo host, or use `https://` URLs directly.

---

## Package Manager

Packages are `.pkg` files — standard ZIP archives with a `package.cfg` metadata file inside.

### Package metadata (`package.cfg`)

| Key | Description |
|-----|-------------|
| `pkg.name` | Display name |
| `pkg.dev` | Author |
| `pkg.ver` | Version string |
| `pkg.dir` | Install path (e.g. `/Packages/HelloWorld`) |
| `pkg.desc` | Short description |
| `pkg.cmd` | Shell command: `name:/path/handler.py:function` |

### Commands

| Command | Description |
|---------|-------------|
| `pkg available` | List all packages in the repo cache |
| `pkg search <query>` | Search cache by name or description |
| `pkg install <name>` | Install from repo cache |
| `pkg install <file.pkg>` | Install a local archive |
| `pkg remove <name>` | Uninstall |
| `pkg list` | Installed packages |
| `pkg info <name>` | Details for an installed package |
| `pkg update` | Refresh repo indexes from the network |
| `pkg upgrade` | Upgrade installed packages that have newer versions in cache |
| `pkg repo list` | Configured repo URLs |
| `pkg repo add <url>` | Add a repo |
| `pkg repo remove <url>` | Remove a repo |

Commands installed by packages are available immediately after install. They disappear immediately after removal. No reboot required either way.

### Quick start

```
pkg repo add http://rpc.novalabs.app/repo/index.json
pkg update
pkg available
pkg install HelloWorld
```

### Repo index format

Repos are JSON files hosted anywhere (GitHub raw URLs work):

```json
{
  "name": "My Repo",
  "packages": [
    {
      "name": "HelloWorld",
      "ver": "1.0.0",
      "author": "dash1101",
      "desc": "Sample package",
      "url": "https://..."
    }
  ]
}
```

### Building packages

`repo/make_pkg.py` is a PC-side script that builds a `.pkg` from a source directory:

```
python repo/make_pkg.py MyPackage/
```

Packages must use `ZIP_STORED` (no compression) — `make_pkg.py` handles this automatically. If you build with a standard ZIP tool using deflate, install will fail unless the target firmware has `zlib`.

---

## Editor

The built-in terminal editor is invoked with:

```
edit myfile.py
nano /Core/somefile.py
vi                         # scratch buffer, no file
```

**Controls:**

| Key | Action |
|-----|--------|
| Arrow keys | Move cursor |
| Ctrl+S | Save |
| Ctrl+Q | Quit |
| Ctrl+X | Save and quit |
| Ctrl+K | Cut line |
| Ctrl+U | Paste line |
| Ctrl+F | Find |
| Ctrl+G | Go to line number |

Requires a real terminal emulator. Thonny's REPL doesn't render ANSI escape sequences correctly.

---

## Settings Panel

Run `settings` to open an interactive panel:

```
┌──────────────────────────────────────────────────────────────┐
│  RPCortex Settings               125 MHz  27.4C  92 KB      │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  SYSTEM                                                      │
│  [1] Verbose Boot         : OFF                              │
│  [2] Program Execution    : ON                               │
│                                                              │
│  HARDWARE                                                    │
│  [3] Boot Overclock       : OFF                              │
│  [4] Beeper               : OFF                              │
│  [5] SD Card Support      : OFF                              │
│                                                              │
│  NETWORK                                                     │
│  [6] WiFi Autoconnect     : ON                               │
│                                                              │
│  [1-6] toggle   [r] refresh   [q] quit                       │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

`[1–6]` toggle the setting. `[r]` refreshes the display (picks up temperature/RAM changes). `[q]` exits and saves.

| # | Setting | Registry key |
|---|---------|-------------|
| 1 | Verbose Boot | `Settings.Verbose_Boot` |
| 2 | Program Execution | `Features.Program_Execution` |
| 3 | Boot Overclock | `Settings.OC_On_Boot` |
| 4 | Beeper | `Features.beeper` |
| 5 | SD Card Support | `Features.SD_Support` |
| 6 | WiFi Autoconnect | `Settings.Network_Autoconnect` |

---

## Registry

The registry lives at `/Nebula/Registry/registry.cfg` — a plain INI file. It's created from an embedded template on first boot if missing.

### Key reference

**[Settings]**

| Key | Values | Description |
|-----|--------|-------------|
| `Setup` | `true/false` | First-run wizard completed |
| `OC_On_Boot` | `true/false` | Apply max clock on every boot |
| `Verbose_Boot` | `true/false` | Show POST info messages |
| `Network_Autoconnect` | `true/false` | WiFi autoconnect on boot |
| `Active_User` | string | Current session user |
| `Startup` | `0 / 6` | Crash sentinel for clock calibration |
| `Version` | string | OS version |

**[Hardware]**

| Key | Description |
|-----|-------------|
| `Clockable` | `true` after clock calibration |
| `Min_Clock` | Minimum safe CPU clock (e.g. `30.0MHz`) |
| `Max_Clock` | Maximum safe CPU clock (e.g. `220.0MHz`) |
| `beeper_pin` | GPIO pin for beeper, or `None` |

**[Networks]** — WiFi credentials (up to 2 slots)

**[Features]**

| Key | Default | Description |
|-----|---------|-------------|
| `Program_Execution` | `true` | Allow running scripts with `exec` |
| `Serial` | `true` | Serial interface active |
| `SD_Support` | `false` | SD card (not yet implemented) |
| `Nova` | `false` | Nova GUI (future) |

### Shell access

```
reg get Settings.OC_On_Boot        # read a key
reg set Settings.OC_On_Boot true   # write a key
env                                 # dump all sections
env Settings                        # dump one section
```

---

## POST

POST runs on every boot before the login prompt. It prints status for each check and stops if a critical check fails.

| Check | Critical | Notes |
|-------|----------|-------|
| Registry | Yes | Creates registry from template if missing |
| CPU arithmetic | Yes | Float, int, comparison, bitwise |
| RAM | Yes | Allocates and verifies a test buffer |
| Clock calibration | No | Runs once; RP2 gets 220 MHz max without probing |
| WLAN | No | Checks for WiFi hardware; autoconnects if configured |
| Beeper | No | Initializes GPIO beeper if configured |

**Boot overclock:** if `Settings.OC_On_Boot = true`, POST applies `Hardware.Max_Clock` via `machine.freq()` on every boot.

**Verbose boot:** `Settings.Verbose_Boot = false` by default — POST only prints warnings and errors. Enable it to see all informational messages.

**Crash sentinel:** POST writes `6` to `Settings.Startup` before clock calibration and clears it on success. If the device crashes mid-calibration, the next boot detects the `6` and disables clocking.

---

## Session Logging

Every session is logged to `/Nebula/Logs/latest.log`. The log captures everything printed through the OS output functions (`ok`, `info`, `warn`, `error`, `fatal`).

Log rotation on each boot:
```
latest.log → log_1.log → log_2.log → ... → log_9.log (dropped)
```

Up to 10 logs are kept. The `/Nebula/Logs/` directory must exist — create it manually if log writes fail:

```
mkdir /Nebula/Logs
```

---

## Hardware & Performance

### Overclocking

RPCortex detects the CPU's safe clock range on first boot. For RP2040/RP2350 the stored max is 220 MHz.

```
pulse oc 220       # overclock for this session
pulse boot on      # apply on every boot
settings           # toggle via panel
```

### NebulaMark

`bench` runs the NebulaMark suite: integer ops, floating-point ops, Mandelbrot iteration, and Pi approximation. Results are printed in operations per second and calculation time.

### Heap and fragmentation

`freeup` clears the command cache and runs garbage collection. Use it before heavy network operations or when you get a `MemoryError` despite apparent free RAM. `meminfo` shows the current heap state.

---

## Supported Hardware

| Board | Status | RAM |
|-------|--------|-----|
| Raspberry Pi Pico (RP2040) | ✅ Primary target | 264 KB |
| Raspberry Pi Pico W (RP2040 + WiFi) | ✅ Full support | 264 KB |
| Raspberry Pi Pico 2 (RP2350) | ✅ Supported | 520 KB |
| Raspberry Pi Pico 2 W (RP2350 + WiFi) | ✅ Supported | 520 KB |
| ESP32 / ESP32-S2 / ESP32-S3 | ✅ Confirmed working | Varies |

---

## Security

- **Passwords** use salted SHA256. Each account has a unique random salt — rainbow tables don't apply. Accounts created before v0.8.1 used unsalted SHA256 and upgrade on the next password change.
- **WiFi credentials** are stored in plaintext in `registry.cfg`. There is no secure storage mechanism on this hardware.
- **Home directories** provide logical separation at `/Users/<username>/`. There is no kernel-enforced permission model.

---

## Known Limitations

- **No real-time clock on base Pico** — `date` shows time since boot epoch until the RTC is set manually
- **HTTPS on Pico 1 W** — TLS needs ~9.5KB contiguous heap; run `freeup` first if the heap is fragmented
- **Editor requires a real terminal** — Thonny REPL won't render it
- **Log directory** — `/Nebula/Logs/` is not created automatically; log writes silently fail if it's missing
- **Dual user store** — `usrmgmt.py` (active) and `regedit.py` (unused legacy XOR store) are separate; unification is planned
- **No tab completion** — planned
- **SD card support** — registry key exists, implementation pending

---

*RPCortex Nebula v0.8.1 — by dash1101*
