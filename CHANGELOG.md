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

## [v0.9.1] "Pulsar" — in development

The shell-power + automation release. Pipes, conditional chaining, a scripting
language, scheduled/unattended tasks, expanded recovery tooling, and the
hardware/local packages a standalone device wants. The headline pieces all build
on a new per-command exit-code + output-capture convention (the item deferred
out of v0.9.0).

### Added — shell power
- **Pipes** — `cmd1 | cmd2 | cmd3`. Each stage's `multi()` (data) output is captured and fed to the next stage as stdin; status lines (`ok`/`info`/`warn`/`error`) still print, like stderr. Consumers `grep`, `wc`, `sort`, `uniq`, `cat`/`read`, `head`, `tail` all read piped stdin when no file argument is given. e.g. `cat /Nebula/Logs/latest.log | grep ERROR | wc`.
- **Conditional chaining** — `cmd1 && cmd2` (run second only if first succeeded) and `cmd1 || cmd2` (only if first failed), combinable with `;` and `|`. Exit status is derived from whether the command called `error()`/`fatal()` — no per-command changes needed.
- **Scripting (`.rps`)** — `script <file.rps>`: variables (`set NAME VALUE`, `$NAME` expansion), `if`/`else`/`end`, `while`/`end`, `#` comments, and any shell command (pipes/chaining included). Conditions are builtins (`eq`, `ne`, `exists`, `empty`) or any command's exit status. New file: `Core/Launchpad/sys_script.py`.

### Added — autonomy
- **Startup tasks** — commands in `/Nebula/Registry/startup.cfg` run once after login, before the prompt, via `launchpad._run_startup_tasks()`. Managed with `startup list|add <cmd>|remove <n>|clear|run`. (New file: `Core/Launchpad/sys_task.py`.)
- **Scheduled & unattended tasks** — `task add <secs> <cmd>` / `list` / `remove <n>` / `clear`, stored in `/Nebula/Registry/tasks.cfg`. `task run` enters a foreground scheduler that fires due tasks on a software-uptime timer and stays responsive (quit with `q`/Ctrl+C via `select()`). Run it as a startup task (`startup add task run`) for a headless, autonomous device.

### Added — recovery & diagnostics
- **`fscheck`** — verify core OS files exist and are non-empty. **`diag`** — RAM/flash/registry/version snapshot. **`logdump [n]`** — print the session log. **`regreset`** — delete `registry.cfg` so POST rebuilds defaults (keeps users + WiFi). **`pkgdisable`/`pkgenable <name>`** — quarantine a misbehaving package without removing it. Registered via their own `recovery.lp` so they load even if `system.lp` is damaged. (New file: `Core/Launchpad/sys_recovery.py`.)

### Added — packages (install with `pkg install <name>`)
- **Calc** — offline calculator; sandboxed math eval (no filesystem/imports) + `hex`/`bin`/`oct`/`dec` conversion.
- **Gpio** — direct pin control: `gpio read|set|toggle|pwm|stop|adc <pin>` on RP2040/RP2350/ESP32.
- **I2CScan** — `i2cscan [scl] [sda]` probes the I2C bus (SoftI2C, any pins) and names common devices.

### Added — networking & personalisation
- **Download progress bars** — `wget` shows `[####----] 47%  N/M B` using `Content-Length` (redraws only on percent change; byte counter when size is unknown).
- **Extended `curl` flags** — `-X <method>`, `-d <data>`, `-H '<header>'`, `-o <file>`, `-s` (silent), `-I` (headers only), `--timeout <secs>`. The default GET-to-stdout behaviour (with redirect following) is unchanged.
- **Personalisation** — `System.Owner` (shown in `sysinfo`, prompted at first-run setup) and `System.TZ_Offset` (hours; applied to `date` output). Device name was already configurable via `System.Device_ID`.

### Changed
- **Unified dispatch & line execution** — `launchpad._dispatch_line()` is the single command router (alias/tilde/`--help`/route), returning a pass/fail status; `_run_line()` handles `;`, `&&`, `||`, and `|` for the interactive loop, recovery loop, startup runner, scripts, and `watch`. Replaces the three copied dispatch blocks from before.
- **`OS_VERSION` → `v0.9.1`**; registry template + boot-file headers bumped to Pulsar.

### Fixed
- Stale "RPCortex Nebula" brand strings in the `help` banner, `sysinfo`, and HTTP `User-Agent` updated to "Pulsar".

---

## [v0.9.0] "Pulsar" - 2026-06-10

First release of the **Pulsar** (β9) series and the first public release since
v0.8.1 — it supersedes v0.8.1 directly. (The v0.8.2 work was an internal
milestone that was never published as its own release; all of it is folded in
here.) A stability-and-quality-of-life release: every new feature is
self-contained and low-risk. Shell pipes and `&&`/`||` were intentionally
deferred — they require a per-command exit-code convention that would be
bug-prone to bolt on now.

### Added
- **Persistent aliases** — aliases now survive reboots, stored in `/Nebula/Registry/aliases.cfg` (one `name=command` per line). Loaded at shell start; saved on every `alias`/`unalias`. Critical built-ins still cannot be shadowed.
- **`du [path]`** — report the total size of a file or directory tree (recursive).
- **`watch [-n <secs>] <command>`** — re-run a command periodically (default every 2 s) until Ctrl+C; clears the screen between runs.
- **`date set YYYY-MM-DD [HH:MM:SS]`** — set the hardware RTC via `machine.RTC()`, so session-log timestamps are finally correct. Bare `date` still prints the current time.
- **Configurable prompt hostname** — the shell prompt host (`user@<host>`) now reads `System.Device_ID`; change it with `reg set System.Device_ID <name>`. Defaults to `pulsar`.
- **OTA update system** — `update check` downloads `rpc.novalabs.app/releases/latest.json` and compares against the running version; `update online` streams and installs the latest `.rpc` over WiFi; `update online --force` reinstalls even if already current.
- **`OS_VERSION` and `OS_CODENAME` constants** (`Core/RPCortex.py`) — single source of truth for the running version and release name; `initialization.start()` syncs `Settings.Version` and `System.Codename` to them on every boot, so the registry can never drift after an OTA update and `ver`/`fetch` stay correct.
- **`_repair_programs_lp()`** in `launchpad.py` — called at shell init; re-adds any missing built-in `programs.lp` entries (fetch, neofetch, bench, pkg, wifi) if the target file exists; prevents silent loss of built-in commands after an edge-case clear.
- **PicoFetch package** (`/Packages/PicoFetch/`) — `fetch` / `neofetch` moved out of `Core/` into a proper package; now upgradeable via `pkg upgrade PicoFetch`.
- **NebulaMark package** (`/Packages/NebulaMark/`) — `bench` extracted from `pulse.py` into a proper package; now upgradeable via `pkg upgrade NebulaMark`.
- **Build tooling** — `build_v090.py` builds the `.rpc` + `.pkg` artifacts; `temp_claude/build_images.py` stages source and portable-`.mpy` deploy images.

### Changed
- **`cp` and `mv` stream large files** — `_copy_chunked()` copies in 1 KB chunks instead of reading the whole file into RAM, fixing OOM on large files on a 264 KB Pico. `mv` now tries `uos.rename()` first (zero-copy, instant on the same filesystem) and only falls back to streamed copy + delete across filesystems.
- **`cp`/`mv`/`rename` accept relative paths** — resolved against the current directory via `_abspath()`; absolute paths still work. All three guard against identical source and destination.
- **`read`/`cat` accept multiple files** — `cat a.txt b.txt` prints each with a `==> name <==` header.
- **Ghost (inline) completion** — per-keystroke completion now only scans the command dict (zero I/O); full path completion (`uos.listdir`) only fires on an explicit Tab press; eliminates typing lag at 115200 baud.
- **Batched log flushing** — `_log_write()` flushes every 8 writes; ERROR/FATAL/WARN still flush immediately for crash-log safety; eliminates serial output lag (~10 ms per flush on LittleFS).
- **`pkg upgrade`** — calls `uninstall(force=True)` so builtin-tagged packages can be upgraded and registry keys are preserved across the uninstall/reinstall cycle.
- **`make_pkg.py`** — now filters `__pycache__/` and `.pyc` files from `.pkg` archives.
- **`compile.bat`** — now also copies the PicoFetch and NebulaMark package dirs into compiled builds (they were silently omitted before).
- **Rebrand β8 "Nebula" → β9 "Pulsar"** — boot banner, registry template (`System.Codename`, `Settings.Version`), `system.lp` header. Built-in packages Launchpad and Editor bumped to 0.9.0; `programs.lp`/`system.lp` realigned for the packaged `fetch`/`bench`.
- **`.rpc` format** — now includes full `Packages/**` source (not just `package.cfg` stubs). Release artifact is `RPC-Pulsar-b9-Stable.rpc`.
- **Release hosting** — `.rpc` release assets are GitHub Release assets; `latest.json` `url` points to `github.com/dash1101/RPCortex/releases/download/...`. The website (`rpc.novalabs.app`) is GitHub Pages, so a push deploys it.

### Fixed
- **`wifi connect <ssid>` never used saved passwords** — `_connect()` in `wifi.py` still looked up the old 2-slot registry keys (`Networks.WiFi_SSID_1/2`) removed in v0.8.1; it now reads `/Nebula/Registry/networks.cfg` via `net._read_networks()`, so known networks connect without re-prompting.
- **"Previous session ended unexpectedly" warning shown on every boot** — POST arms `Settings.Startup = "1"` (session-active sentinel) at its end, and `initialization.start()` then read that same key for the startup banner, so the crash warning fired on every boot, clean or not. The banner now uses the pre-POST value captured in `post.boot_startup_mode`; this also makes the update-failed / safe-mode banners (modes 3/4/5) reachable for the first time.
- **Built-in package stubs restored** — `/Packages/Launchpad/package.cfg` and `/Packages/Editor/package.cfg` were deleted in an old history rewrite and never re-added; fresh installs showed no built-ins in `pkg list`.
- **`masked_inpt` on all password flows** — `mkacct`, `rmuser`, `chpswd`, `change_password`, and `wifi add` all use masked input now; `usrmgmt.login_seq()` login is masked too.
- **`watch` reaches the running shell engine** via `sys.modules['Core.launchpad']` rather than a bare `import launchpad`, which would have created a second module instance with an empty command table.

### Removed
- `bench` and `fetch` hard-coded functions removed from `sys_sys.py` and `pulse.py` respectively (moved to packages).
- Orphaned color constants (`HEADER`, `OKBLUE`, …) and `tr = []` removed from `pulse.py`; the old `NebulaMark()` body now lives in `/Packages/NebulaMark/nebulamark.py`.

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
