# Changelog

All notable changes to RPCortex are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)

---

<!-- HOW TO ADD AN ENTRY
  1. Add your changes under [Unreleased] as you work.
  2. When releasing, rename [Unreleased] to [vX.Y.Z] - YYYY-MM-DD
     and add a new blank [Unreleased] section at the top.
  3. Categories: Added / Changed / Fixed / Removed / Security
-->

## [Unreleased]

---

## [v0.8.1-beta2] - 2026-03-27

### Added
- **Tab completion** — ghost text (dim gray suffix) for single-match command prefixes; Tab accepts
- **Shell aliases** — `alias name=cmd` / `unalias` / bare `alias` lists all; session-local, in `_CRITICAL` so always available
- **`_xfer` serial protocol** — built-in base64 file transfer from browser to running shell; no raw REPL, no WiFi required
- **`update from-file <path>`** — apply a `.rpc` update archive while preserving user accounts, WiFi, and config
- **`factoryreset`** — wipe users/packages/logs; OS untouched; reboots into first-run setup (type `CONFIRM`)
- **`reinstall [path.rpc]`** — full OS wipe + optional auto-install stub (type `WIPE`)
- **Browser update page** (`update.html`) — push a `.rpc` update from a browser tab over USB; no WiFi, no raw REPL
- **Web installer wipe step** — device filesystem cleared before install for a guaranteed clean slate
- **Wipe confirmation checkbox** in web installer — must be acknowledged before Install Now is enabled
- **Settings TUI** (`settings` command) — ANSI box-drawing panel; toggles Verbose Boot, OC on Boot, Autoconnect, beeper, and more
- **`Settings.Verbose_Boot`** registry key — suppress POST `info()` messages when false
- **`Settings.OC_On_Boot`** registry key — apply stored overclock automatically on every boot
- **Log directory auto-created** by POST on first boot (`/Nebula/Logs/`)
- **Official repo auto-added** on first-run setup; `guest` account created silently
- **Post-update login banner** — shows new version after successful `update from-file`
- **`rpc_install.py`** — streaming Central Directory ZIP extractor; accurate file sizes regardless of data-descriptor flag
- **`rpc_stub.py`** — self-contained reinstall boot stub; no `Core/` imports needed

### Changed
- **Registry config caching** — `regedit.py` caches parsed config in memory; repeated `read()` calls no longer touch disk
- **Web package browser** (`packages.html`) — updated to use `_xfer` protocol instead of raw REPL; installs to running shell
- **Shell built-in commands** now loaded via `__import__()` instead of `exec()` — modules cached in `sys.modules`, heap-free on retry
- **MicroPython minimum version** raised to v1.25+ (v1.28 recommended)
- **Markdown docs** moved to `website/github/` subfolder; `release.md` renamed to `release (v0.8.1-beta2).md`
- **All f-strings** in `regedit.py` replaced with `.format()` for firmware compatibility
- **Redundant `gc.collect()` calls** reduced across codebase

### Fixed
- `ls` no longer changes CWD when given a path argument
- POST beeper crash (`beeper` was `None` when setting was enabled)
- POST `check_registry()` crash when `/Nebula/` parent directory was missing
- `recovery_mode()` crash in `initialization.py` (called undefined `recovery()`)
- Startup mode `7` (boot-clock crash sentinel) now shows a login notification
- `update from-file` extracting wrong file count — switched to Central Directory approach
- Files extracted to wrong paths — prefix detection now checks all entries, not just the first
- `programs.lp` no longer overwritten by `update from-file` (user package entries preserved)
- Post-update login banner now shows actual version from `Settings.Version`

### Removed
- XOR-encrypted user store dead code from `regedit.py` (~115 lines)
- Unused `import random` from `main.py` and `pulse.py`
- "Unified user system" from backlog — `usrmgmt.py`/`user.cfg` was already the only active user store

---

## [v0.8.0-rc4] - 2026-03-01

### Added
- HTTP client v2 — fully iterative redirect following, 15s socket timeout
- HTTPS support — `ssl.wrap_socket` with `ussl` fallback; heap-consolidation nudge for Pico 1 W
- Package manager (`pkg`) — install, remove, upgrade, search, repo management
- WiFi shell commands — `wifi connect/scan/disconnect/list/add/forget`
- Cursor navigation in shell — left/right arrows, Home/End, Ctrl+A/E, Delete-forward
- Home directory support — `~` shorthand, `cd ~`, `~/path` resolution
- Salted SHA256 passwords in `usrmgmt.py`
- First-run setup wizard — creates `root` and `guest` accounts
- Web package browser — install packages from browser (used raw REPL in rc4)
- `freeup` command — compacts heap, reports delta
- `settings` TUI stub
- `MemoryError` auto-recovery in shell dispatch loop

### Fixed
- `split(maxsplit=N)` keyword arg crash — all calls use positional form
- `system.py` OOM crash — split into focused `sys_*.py` files
- WiFi key names in registry
- `ls` missing SIZE column
- POST RAM test hanging on large-RAM boards

---

## [v0.8.0-beta] - 2026-02-01

### Added
- Initial Nebula release
- Interactive shell (`launchpad.py`) with history navigation
- Filesystem commands (`ls`, `cd`, `cp`, `mv`, `rm`, `tree`, `df`)
- User management (`mkacct`, `rmuser`, `chpswd`, `logout`)
- POST — registry check, CPU arithmetic test, RAM stress test, clock calibration
- Registry (`regedit.py`) — INI-style config with dot-notation API
- Text editor (`editor.py`) — nano-style ANSI terminal editor
- System info (`fetch` / `picofetch.py`)
- Hardware management (`pulse`) — overclock, underclock, NebulaMark benchmark
- Session logging to `/Nebula/Logs/`
- WiFi networking (`net.py`) — scan, connect, saved networks, autoconnect
- `wget`, `curl`, `runurl`, `ping`, `nslookup`

---

*RPCortex by [dash1101](https://github.com/dash1101). MIT License.*
