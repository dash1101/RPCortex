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

A lightweight CLI operating system for the Raspberry Pi Pico and RP2040/RP2350-based boards, written entirely in MicroPython. Think of it as a Linux-flavored shell running on a $4 microcontroller — real user accounts, a package manager, WiFi, a text editor, and a proper boot sequence, all on hardware with 264KB of RAM.

> **v0.8.1-alpha** — Active development. Most features work. Some rough edges remain. Targeting a release candidate in mid-2026.

---

## Features

- **Interactive CLI shell** — colored prompt, command history, up/down arrow navigation, session logging
- **Full filesystem commands** — `ls`, `cd`, `mv`, `cp`, `rm`, `tree`, `df`, and more
- **User accounts** — salted SHA256 passwords, multi-user support, account creation/removal from the shell
- **WiFi networking** — scan, connect, saved networks, autoconnect on boot (Pico W and ESP32)
- **HTTP/HTTPS downloads** — `wget` streams directly to flash; `runurl` downloads and executes a Python file immediately
- **Apt-style package manager** — install/remove/upgrade packages, configure repos, search the repo cache
- **Built-in text editor** — nano-style editor via `edit` or `nano` (requires serial terminal)
- **POST (Power-On Self Test)** — registry check, CPU test, RAM stress test, clock detection, WiFi autoconnect
- **Registry** — INI-style persistent config for system settings, network credentials, hardware info
- **Session logging** — every boot session logged to `/Nebula/Logs/`, rotated up to 10 logs
- **Recovery shell** — unauthenticated fallback shell if the main boot sequence fails
- **Neofetch-style system info** — `fetch` shows board, CPU, RAM, flash, uptime, UID

---

## Supported Hardware

| Board | Status |
|-------|--------|
| Raspberry Pi Pico (RP2040) | ✅ Primary target |
| Raspberry Pi Pico W (RP2040 + WiFi) | ✅ WiFi supported |
| Raspberry Pi Pico 2 (RP2350) | ✅ Supported |
| Raspberry Pi Pico 2 W (RP2350 + WiFi) | ✅ Should work |
| ESP32 / ESP32-S2 / ESP32-S3 | 🔶 Partial support |

---

## Getting Started

1. Flash MicroPython firmware to your board
2. Copy all files from this repo onto the board's filesystem
3. Connect via serial terminal at **115200 baud** (PuTTY recommended)
4. Reboot — `main.py` runs automatically
5. POST runs, then you're prompted to create an account on first boot
6. Log in and you're in the shell

**Note:** Thonny's REPL works but has quirks (no arrow key history, double-printed input). Use PuTTY or a similar terminal for the best experience.

---

## Package Manager

Packages are `.pkg` files (standard ZIP archives). Install from a local file or directly by name from a configured repo:

```
pkg repo add https://raw.githubusercontent.com/dash1101/RPCortex/main/repo/index.json
pkg update
pkg search hello
pkg install HelloWorld
pkg list
pkg upgrade
```

A `make_pkg.py` build script is included in `repo/` for creating packages from a source directory on your PC.

---

## Shell Preview

```
dash@nebula:/>
dash@nebula:/> wifi status
[i] Connected  SSID: MyNetwork  IP: 192.168.1.42
dash@nebula:/> pkg search hello
  NAME                 VERSION    AUTHOR       DESCRIPTION
  ---------------------------------------------------------------
  HelloWorld           1.0.0      dash1101     Sample demo package
dash@nebula:/> pkg install HelloWorld
[i] Found 'HelloWorld'. Downloading...
[OK] Package 'HelloWorld' v1.0.0 installed.
dash@nebula:/> hello
  Hello from HelloWorld!
  RPCortex package system is working correctly.
```

---

## Status & Roadmap

| Phase | Status | Notes |
|-------|--------|-------|
| Initiation | ✅ Complete | March 2025 |
| Planning | ✅ Complete | March 2025 |
| Development | 🏗️ In Progress | Active — March 2026 |
| Testing | 🧪 Upcoming | TBD |
| Release Candidate | 🚀 Targeted | Mid-2026 |

---

## Recommended System Requirements

- **MicroPython** v1.20 or newer
- **Flash:** 4MB (2MB minimum)
- **RAM:** 264KB (Pico 1) or better

---

## License

RPCortex is open-source. Explicit credit to **[@dash1101](https://github.com/dash1101)** is required for use in public projects. See the [LICENSE](https://github.com/dash1101/RPCortex/blob/main/LICENSE) file for details.

---

## Resources

- 📖 [Documentation / Wiki](https://github.com/dash1101/RPCortex/wiki)
- 🐛 [Issue Tracker](https://github.com/dash1101/RPCortex/issues)
- 💬 [Discussions](https://github.com/dash1101/RPCortex/discussions)
- 🔥 [Releases](https://github.com/dash1101/RPCortex/releases)

---

###### RPC-β8X (v0.8.1-alpha) — Nebula
