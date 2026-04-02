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

## [v0.8.1] - 2026-04-02

### Added
- **`--help` / `-h` flag** — passing `--help` or `-h` to any built-in command redirects to `help <command>`; individual command hints added to `help` as well
- **Unlimited saved WiFi networks** — saved networks moved from the 2-slot registry to `/Nebula/Registry/networks.cfg` (tab-separated, no limit)
- **Auto-save WiFi on connect** — `wifi connect` now automatically adds the network to saved networks; idempotent (updates existing entry)
- **`pkg commands` subcommand** — lists all shell commands registered by installed packages (reads `programs.lp`)
- **`recovery` command** — enter recovery mode (unauthenticated shell) from a running session without triggering a boot error
- **Extraction progress in `update from-file`** — pre-counts wanted files and shows `[n/total]` progress counter during archive extraction
- **`Device_ID` in registry and `fetch`** — `System.Device_ID` added to factory registry template; shown in `fetch` / `picofetch` output

### Changed
- `_cmd_hints` lookup added to `help` — `help <command>` now shows a one-line description for any known command name (not just category names)

---

## [v0.8.1-rc1] - 2026-04-01

### Added
- **Tilde expansion everywhere** — `~` and `~/path` now expand to the user home in all command arguments, not just `cd`
- **`Program_Execution` setting is functional** — when set to `false` via `settings` or `reg set`, the `exec` command and script fall-through are blocked; clear error shown with re-enable instructions

### Fixed
- **Boot clock disabled after every boot** — `_apply_boot_clock()` never cleared the `"7"` crash sentinel on success; every clean boot was mistakenly treated as a crash on the *next* boot, silently disabling `OC_On_Boot`. Sentinel is now cleared immediately after `machine.freq()` succeeds.
- **NOPASS login (guest) left crash sentinel alive** — logging in as a NOPASS account (e.g. `guest`) did not save `Settings.Startup = "0"`; any pending sentinel from POST clock setup persisted to the next boot. Fixed.

---

## [v0.8.1-beta4] - 2026-03-31

### Added
- **Password masking** — `masked_inpt()` in `RPCortex.py`; login and setup prompts now echo `•` instead of characters
- **Case-insensitive `cd`** — on `OSError`, scans parent directory for a case-insensitive match before giving up
- **`echo` output redirection** — `echo text > file` (overwrite) and `echo text >> file` (append)
- **`.mpy` fallback in POST** — `check_core()` and `check_pulse()` accept `.mpy` for compiled builds
- **Low-RAM warning** — shell warns once when free RAM drops below 70 KB after a command dispatch
- **`pulse boot` improvements** — `pulse boot <MHz>` sets and enables in one step; `on`/`off`/`MHz` all handled

### Fixed
- `rm` single-file prompt is now `y/n` only (no `(a)` option for non-recursive deletes)
- Recovery mode startup message corrected — mode `"1"` now says "unexpected shutdown" not "recovery requested"

---

## [v0.8.1-beta3] - 2026-03-28

### Added
- **Tab completion** — ghost text (dim gray suffix) for single-match command prefixes; Tab accepts; path completion on arguments after first word
- **Shell aliases** — `alias name=cmd` / `unalias` / bare `alias` lists all; session-local, in `_CRITICAL` so always available regardless of heap state
- **Multi-command lines** — `cmd1; cmd2; cmd3` on one line; `_split_cmds()` is quote-aware
- **`grep`, `wc`, `find`, `sort`, `uniq`, `hex`, `basename`, `dirname`** — new text-processing commands in `sys_text.py`
- **`sleep <secs>`** — pause shell; supports decimal values
- **`which <cmd>`** — show where a command is defined (critical built-in, registered command, or alias)
- **`rawrepl`** — raises `SystemExit(0)` to exit OS to MicroPython REPL; use before Web Installer without a full wipe
- **`settings` TUI** — ANSI box-drawing panel; toggles Verbose Boot, OC on Boot, Autoconnect, beeper, SD Support, Program Execution
- **`_xfer` serial protocol** — built-in base64 file transfer from browser; no raw REPL, no WiFi required
- **`update from-file <path>`** — apply a `.rpc` update archive preserving user accounts, WiFi, and config
- **`factoryreset`** — wipe users/packages/logs, reset registry; reboots into first-run setup (type `CONFIRM`)
- **`reinstall [path.rpc]`** — full OS wipe + optional auto-install stub (type `WIPE`)
- **Browser update page** (`update.html`) — push a `.rpc` update from a browser tab over USB; no WiFi, no raw REPL
- **Roadmap page** (`roadmap.html`) — linked from all nav bars
- **Web installer version picker** — driven by `releases/releases.json`; add new releases via JSON, no HTML edit needed
- **OS Update page version picker** — driven by `releases/updates.json`

### Changed
- Shell built-in commands loaded via `__import__()` instead of `exec()` — cached in `sys.modules`, zero re-compile cost on retry
- `_get_scope` no longer re-injects shell state on every cached command call — 6 setattr calls saved per dispatch
- `MemoryError` recovery nudge added (alloc 4 KB → free → gc) to consolidate fragmented heap before retry
- `rm` y/n/a/c fixed — `a` applies to all subsequent, `c` cancels all, `n` correctly prevents parent dir removal
- Tab completion dir-detection uses `uos.stat()` not `uos.listdir()`

### Fixed
- `logout()` dead fallback branch importing from wrong module — removed
- `echo`/`say` usage message corrected
- Duplicate `gc.collect()` removed from `pkg update`
- CTRL+C at login no longer reboots (removed outer restart loop from `main.py`)
- Shell starts in user home dir; prompt shows `~` / `~/sub` Linux-style
- `ls` path argument no longer permanently changes CWD

### Removed
- XOR-encrypted user store dead code from `regedit.py` (~115 lines)
- `/Core/Launchpad/system.py` legacy stub
- `Core/PMS.py` dead file with broken imports

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
