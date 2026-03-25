   <a href="https://github.com/dash1101/RPCortex">
     <p align="center">
       <img src="Assets/RPCortex/RPCortex.png" alt="RPCortex Logo">
     </p>
   </a>

   <a href="https://github.com/dash1101/RPCortex"><img src="https://img.shields.io/github/v/release/dash1101/RPCortex?include_prereleases&label=Latest%20Release"></a>
   <a href="https://github.com/dash1101/RPCortex/issues"><img src="https://img.shields.io/github/issues/dash1101/RPCortex"></a>
   <a href="https://github.com/dash1101/RPCortex/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-orange"></a>

---

# RPCortex — Nebula β8X

RPCortex is a CLI operating system for the Raspberry Pi Pico, written entirely in MicroPython. It turns a microcontroller into something that actually behaves like a computer — a real interactive shell, user accounts with passwords, a package manager, WiFi, a text editor, and a structured boot process with hardware checks.

It runs on hardware with 264KB of RAM. That constraint is the point.

---

## What it feels like

You flash MicroPython, copy the files, open PuTTY at 115200 baud, and reboot. POST runs — registry check, CPU test, memory verification, clock calibration, WiFi probe. You set a root password. You log in. The prompt comes up:

```
root@nebula:~>
```

From there you have a shell that works. `ls` shows your files with sizes and timestamps. `cd ~` takes you home. Up/down arrows scroll your last 50 commands. Left/right arrows move the cursor so you can edit a command mid-line. You can open a file, edit it, save it. You can connect to WiFi and download a package by name. You can overclock the CPU, benchmark it, and check the temperature — all from the same prompt.

It's not trying to be Linux. It's a $4 microcontroller running MicroPython. But within those constraints, it behaves like a real system.

---

## What you can do with it

**Use it as a shell environment.** Browse and manage files, run scripts, edit configs. The filesystem commands cover everything standard: `ls`, `cd`, `cp`, `mv`, `rm`, `tree`, `df`. The text editor opens any file over serial.

**Connect to the internet.** WiFi connects on any Pico W or ESP32. `wget` streams files directly to flash. `curl` fetches APIs. `runurl` downloads and executes a Python file in one step. Autoconnect on boot if you want it.

**Install software.** The package manager works like you'd expect: `pkg install <name>`, `pkg remove <name>`, `pkg upgrade`. Packages are listed in repo indexes — add the official repo, run `pkg update`, and everything in the index is a single command away. Installed commands show up in the shell immediately.

**Manage users.** Create accounts with `mkacct`, change passwords with `chpswd`, remove accounts with `rmuser`. Each user gets their own home directory. The `guest` account requires no password.

**Tune the hardware.** `pulse oc 220` overclocks to 220 MHz. `pulse uc 80` underclocks. `bench` runs NebulaMark. `freeup` compacts the heap when things get fragmented after heavy use.

**Configure the system.** `settings` opens an interactive panel where you toggle boot overclocking, WiFi autoconnect, verbose boot, the beeper, and SD card support. `reg get`/`reg set` writes directly to the registry if you need something the panel doesn't cover.

---

## Supported Hardware

| Board | Status |
|-------|--------|
| Raspberry Pi Pico (RP2040) | ✅ Supported |
| Raspberry Pi Pico W (RP2040 + WiFi) | ✅ Supported |
| Raspberry Pi Pico 2 (RP2350) | ✅ Supported |
| Raspberry Pi Pico 2 W (RP2350 + WiFi) | ✅ Recommended |
| ESP32 / ESP32-S2 / ESP32-S3 | ✅ Recommended |

Requires MicroPython v1.20 or newer. v1.27+ recommended. 4MB flash minimum.

---

## Getting Started

1. Flash MicroPython to your board
2. Copy all files from this repo to the board's filesystem
3. Open a serial terminal at **115200 baud** — PuTTY on Windows, minicom or screen on Linux/macOS
4. Reboot — `main.py` runs automatically
5. Set your root password on first boot, then log in

```
wifi connect
pkg repo add http://rpc.novalabs.app/repo/index.json
pkg update
pkg available
pkg install HelloWorld
```

---

## Documentation

- **[NebulaDocs.md](NebulaDocs.md)** — full command reference, shell controls, registry keys, networking guide, package format
- **[release.md](release.md)** — what's in this version
- **[Issues](https://github.com/dash1101/RPCortex/issues)** — bug reports and feature requests
- **[Discussions](https://github.com/dash1101/RPCortex/discussions)** — questions and ideas

---

## License

Open source. Explicit credit to **[@dash1101](https://github.com/dash1101)** is required for use in public projects. See [LICENSE](https://github.com/dash1101/RPCortex/blob/main/LICENSE).

---

###### RPCortex Nebula β8X — v0.8.0-rc
