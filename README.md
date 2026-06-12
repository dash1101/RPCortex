   <a href="https://github.com/dash1101/RPCortex">
     <p align="center">
       <img src="RPCortex.png" alt="RPCortex Logo">
     </p>
   </a>

   <a href="https://github.com/dash1101/RPCortex"><img src="https://img.shields.io/github/v/release/dash1101/RPCortex?include_prereleases&label=Latest%20Release"></a>
   <a href="https://github.com/dash1101/RPCortex/issues"><img src="https://img.shields.io/github/issues/dash1101/RPCortex"></a>
   <a href="https://github.com/dash1101/RPCortex/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-orange"></a>

---

# RPCortex — Pulsar β9

RPCortex is a CLI operating system for the **Raspberry Pi Pico series** (RP2040 / RP2350) and the **ESP32-S3**, written entirely in **MicroPython**. It turns a microcontroller into something that actually behaves like a computer — a real interactive shell with pipes and scripting, user accounts with passwords, a package manager, WiFi, over-the-air updates, a text editor, task automation, and a structured boot process with hardware checks.

It runs on hardware with 264 KB of RAM. That constraint is the point.

---

## What it feels like

You flash MicroPython, copy the files, open PuTTY at 115200 baud, and reboot. POST runs — registry check, CPU test, memory verification, clock calibration, WiFi probe. You set a root password. You log in. The prompt comes up:

```
root@pulsar:~>
```

From there you have a shell that works. `ls` shows your files with sizes and timestamps. `cd ~` takes you home. Up/down arrows scroll your last 50 commands. Left/right arrows move the cursor so you can edit a command mid-line. Tab completes commands and paths. You can pipe commands together (`cat log | grep ERROR | wc`), chain them (`wifi connect && pkg update`), and write scripts. You can open a file, edit it, save it. You can connect to WiFi, update the OS over the air, and install a package by name. You can overclock the CPU, benchmark it, drive GPIO pins, and check the temperature — all from the same prompt.

It's not trying to be Linux. It's a $4 microcontroller running MicroPython. But within those constraints, it behaves like a real system.

---

## What you can do with it

**Use it as a shell environment.** Browse and manage files, run scripts, edit configs. The filesystem commands cover everything standard: `ls`, `cd`, `cp`, `mv`, `rm`, `tree`, `df`, `du`. Copies and moves stream large files safely without exhausting RAM, and accept relative paths. Read several files at once with `cat a.txt b.txt`. Text processing with `grep`, `find`, `sort`, `wc`, and `hex` — and you can **pipe** them together (`cat log | grep ERROR | sort | uniq`) and chain with `&&` / `||`. The text editor opens any file over serial. `~` expands to your home directory in any argument — `cd ~/docs`, `cp ~/config.txt /tmp/`, all of it.

**Automate it.** Write `.rps` scripts with variables, `if`/`else`, and `while` loops, then run them with `script myjob.rps`. Add commands to run automatically at boot (`startup add wifi connect`), or on a repeating timer (`task add 60 sysmon`). Run `startup add task run` and the device boots straight into an autonomous scheduler — no terminal required. It's a standalone computer that can now do things on its own.

**Talk to hardware.** Drive pins straight from the prompt with the `gpio` package — `gpio set 25 high`, `gpio pwm 15 50`, `gpio read 14`, `gpio adc 26`. Scan for I²C sensors with `i2cscan` (it names common parts like SSD1306 and BME280). Do quick math with `calc "3 * (4 + 2)"` and base conversions with `calc hex 255`.

**Connect to the internet.** WiFi connects on any Pico W or ESP32 — and remembers your networks and passwords. `wget` streams files directly to flash. `curl` fetches APIs. `runurl` downloads and executes a Python file in one step. Autoconnect on boot if you want it.

**Update over the air.** `update check` tells you if a newer release is out; `update online` downloads and installs it over WiFi and reboots — your accounts, settings, and installed packages are preserved. No cable, no browser required. Prefer a wire? `update from-file` and the browser update page still work.

**Install software.** The package manager works like you'd expect: `pkg install <name>`, `pkg remove <name>`, `pkg upgrade`. Add the official repo, run `pkg update`, and everything in the index is a single command away. Installed commands show up in the shell immediately — no reboot. Even the built-in `fetch` and `bench` tools are packages now, so they update independently.

**Install packages from your browser.** The [web package browser](https://rpc.novalabs.app/packages.html) installs packages directly to a connected device over USB — no WiFi, no REPL, no reboot. Connect, click Install, done.

**Manage users.** Create accounts with `mkacct`, change passwords with `chpswd`, remove accounts with `rmuser`. Each user gets a home directory; every password prompt is masked. The `guest` account requires no password.

**Tune the hardware.** `pulse set 220` overclocks to 220 MHz. `pulse boot 200` sets a boot clock. `bench` runs the RPCMark benchmark. `freeup` compacts the heap when things get fragmented after heavy use.

**Make it yours.** `settings` opens an interactive panel to toggle boot overclocking, WiFi autoconnect, verbose boot, the beeper, SD support, and program execution. `reg get`/`reg set` writes the registry directly. Aliases (`alias ll=ls -l`) build your own shortcuts — and they survive reboots. `watch -n 5 sysinfo` keeps a live readout on screen. `date set` fixes the clock, and `reg set System.TZ_Offset -5` keeps it in your timezone. Name the device's owner (`reg set System.Owner Dash`) and rename the host in your prompt (`reg set System.Device_ID mypico`).

**Recover from trouble.** If something breaks, recovery mode gives you a limited shell plus repair tools: `fscheck` verifies the core files are intact, `diag` prints a health snapshot, `logdump` shows the session log, `regreset` rebuilds a corrupt registry (keeping your accounts and WiFi), and `pkgdisable` quarantines a misbehaving package without removing it.

---

## Supported Hardware

| Board | Status |
|-------|--------|
| Raspberry Pi Pico 2 W (RP2350 + WiFi) | Recommended |
| ESP32-S3 | Recommended |
| Raspberry Pi Pico (RP2040) | Supported |
| Raspberry Pi Pico W (RP2040 + WiFi) | Supported |
| Raspberry Pi Pico 2 (RP2350) | Supported |
| ESP32 / ESP32-S2 | Supported |

Requires MicroPython v1.25 or newer. v1.28 recommended. 4 MB flash minimum.

---

## Getting Started

**Easiest:** Use the [Web Installer](https://rpc.novalabs.app/install) — flashes RPCortex directly from your browser over USB. No desktop software required (Chrome/Edge only).

**Manual:**

1. Flash MicroPython to your board
2. Copy all files from this repo to the board's filesystem
3. Open a serial terminal at **115200 baud** — PuTTY on Windows, minicom or screen on Linux/macOS
4. Reboot — `main.py` runs automatically
5. Set your root password on first boot, then log in

```
wifi connect
pkg update
pkg install HelloWorld
update check
```

**Smaller, faster image:** a precompiled `.mpy` build (architecture-neutral — one image for RP2040, RP2350, and ESP32) is ~44% smaller and imports faster. Build it with `compile.bat` / `build_images.py` and deploy with `deploy.bat --compiled`.

---

## Documentation

- **[Documentation](https://rpc.novalabs.app/docs)** — full command reference, shell controls, registry keys, networking, and package format
- **[CHANGELOG](https://rpc.novalabs.app/release)** — version history and release notes
- **[ROADMAP](https://rpc.novalabs.app/roadmap)** — what's shipped, what's next, and where ideas land
- **[rpc.novalabs.app](https://rpc.novalabs.app)** — web installer, package browser, OS updater, and HTML docs
- **[Package Dev Guide](https://rpc.novalabs.app/PackageDev)** — build and publish your own packages
- **[Issues](https://github.com/dash1101/RPCortex/issues)** — bug reports and feature requests

---

## License

Open source. Explicit credit to **[@dash1101](https://github.com/dash1101)** is required for use in public projects. See [LICENSE](https://github.com/dash1101/RPCortex/blob/main/LICENSE).

---

###### RPCortex Pulsar β9 — v0.9.1
