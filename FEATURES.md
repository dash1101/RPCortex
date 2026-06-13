# RPCortex — Feature List

**RPCortex Pulsar — v0.9.1** · MicroPython operating system for RP2040 / RP2350
(and ESP32-S3). Serial-first (PuTTY @ 115200). Single source of truth for the
running version lives in `Core/RPCortex.py` (`OS_VERSION` / `OS_CODENAME`).

> This is a capabilities overview. For internals see `CLAUDE.md`; for the
> strategic roadmap see `ROADMAP.md` and [rpc.novalabs.app/roadmap](https://rpc.novalabs.app/roadmap);
> for release notes see [rpc.novalabs.app/release](https://rpc.novalabs.app/release).

---

## 1. Boot & system integrity

- **Power-On Self Test (POST)** — registry validation, CPU arithmetic check,
  fast RAM stress test (single-buffer stride pattern, scales to free RAM), clock
  check, WLAN check + optional autoconnect, beeper init. Returns pass/fail to
  gate boot.
- **First-run setup** — creates `root` (password set interactively) and a
  NOPASS `guest` account, user home directories, and prompts for owner name on
  first boot.
- **Crash sentinels** — `Settings.Startup` markers detect mid-boot crashes
  (clock calibration, boot overclock) and self-heal on the next boot.
- **Clock calibration** — runs once on first boot; RP2040/RP2350 get a safe
  220 MHz max, other platforms are probed.
- **Verbose / quiet boot** — `Settings.Verbose_Boot` toggles info-level POST
  messages.
- **Recovery mode** — if init fails, an auth-free recovery shell launches
  automatically; also reachable on demand with the `recovery` command.

## 2. Launchpad shell

A full interactive CLI engine (`Core/launchpad.py`) loaded from `.lp` command
registries.

- **Linux-style prompt** — `user@host:~>` with `~` home abbreviation; hostname
  configurable via `System.Device_ID`.
- **Line editing** — full cursor navigation (← →, Home/End, Ctrl+A/E),
  insert-at-cursor, Delete-forward.
- **Tab + ghost completion** — zero-I/O per-keystroke command completion (no
  typing lag at high baud); full path completion on explicit Tab.
- **Command history** — recall with ↑ ↓ and the `history` command.
- **Multi-command lines** — `;` separator (quote-aware).
- **Pipes** — `cmd | grep ...` chains; consumers (`grep`/`wc`/`sort`/`uniq`/
  `cat`/`head`/`tail`) read piped stdin.
- **Conditionals** — `&&` / `||` chaining backed by a real exit-status
  convention (`error()`/`fatal()` set the status flag).
- **Persistent aliases** — `alias`/`unalias`, saved to
  `/Pulsar/Registry/aliases.cfg`, survive reboot; critical built-ins can't be
  shadowed.
- **Tilde expansion** everywhere; case-insensitive `cd`.
- **Idle auto-logout** — `Settings.Idle_Logout` (minutes); read live, no
  re-login needed to change it.
- **Resilience** — MemoryError auto-recovery (cache clear + GC + heap nudge +
  retry); critical commands (`reboot`, `freeup`, `gc`, `alias`, `_xfer`, …)
  bypass `exec()` and always work even with a fragmented heap.

## 3. Commands

### Filesystem (`sys_fs.py`)
`ls`/`dir`, `cd`/`chdir`, `pwd`, `touch`/`write`, `mkdir`, `rm`/`del`/`rmdir`,
`read`/`cat`/`view` (multi-file), `head`, `tail`, `exec`, `rename`/`ren`,
`mv`/`move`, `cp`/`copy`, `df`, `du` (recursive size), `tree`.
- Streamed `cp`/`mv` (1 KB chunks — no large-file OOM); `mv` uses `uos.rename`
  fast-path; relative **or** absolute paths everywhere.

### System & control (`sys_sys.py`)
`reboot`, `sreboot`/`softreset`, `sysinfo`, `meminfo`, `uptime`, `date`
(+ `date set` to set the RTC), `watch [-n s] <cmd>`, `ver`, `clear`/`cls`,
`pulse` (clock management), `edit`/`nano`/`vi`, `env`, `reg`, `freeup`/`gc`,
`settings`, `help`, `echo`/`print` (+ `>`/`>>` redirect), `history`, `sleep`,
`which`, `factoryreset`, `reinstall`, `update`.

### Text processing (`sys_text.py`)
`grep` (line-numbered, highlighted), `wc`/`count`, `find` (depth-limited
recursive), `sort`, `uniq`, `hex`/`hexdump`, `basename`, `dirname`.

### Networking (`sys_net.py`)
`wget` (streamed, with download progress bar), `curl` (extended flags:
`-X/-d/-H/-o/-s/-I/--timeout`), `runurl`/`run`, `ping` (TCP RTT), `nslookup`/`ns`.

### Users (`sys_user.py`)
`whoami`, `users` (lists accounts with active/admin/nopass tags), `mkacct`
(`[name] [--nopass] [--admin]`), `usermod` (one command for `passwd` / `rename`
/ `admin on|off` / `nopass on|off`), `passwd [user]` (self-service password
change; admin-gated for another user), `rmuser` (root/guest protected),
`logout`, `exit`. Admin accounts and a `require_admin` re-auth gate forced
actions (e.g. `pkg remove --force`).

### Automation (`sys_task.py`)
- `startup list|add <cmd>|remove <n>|clear|run` — commands run once at login
  (`/Pulsar/Registry/startup.cfg`).
- `task list|add <secs> <cmd>|remove <n>|clear|run` — scheduled tasks on a
  software-uptime timer (`/Pulsar/Registry/tasks.cfg`); `task run` is a
  responsive foreground scheduler (q / Ctrl+C).
- `startup add task run` = headless autonomous mode.

### Scripting (`sys_script.py`)
`script <file.rps>` (or run a `.rps` file by name). Statements: `set`/`$var`,
`inc`/`dec`, `prompt`, `capture NAME <cmd>`, `if`/`else`/`end`, `while`/`end`,
`break`/`continue`, `stop`. Conditions: `eq`/`ne`/`gt`/`lt`/`ge`/`le`,
`contains`, `exists`, `empty`, `not <cond>`, or any shell command as a
condition.

### Recovery & diagnostics (`sys_recovery.py`, own `recovery.lp`)
`fscheck`, `diag`, `logdump [n]`, `regreset` (rebuild registry, keep users),
`pkgdisable`/`pkgenable`. Registered separately so they load even if
`system.lp` is damaged.

## 4. Hardware management (`pulse.py` + `pulse` command)
- `pulse status` / `set <MHz>` / `min` / `max` / `boot <MHz>` / `boot on|off`.
- Boot overclock — `Settings.OC_On_Boot` applies stored `Hardware.Max_Clock`
  on every boot.
- CPU and memory checks; onboard temperature read (RP2040/RP2350).

## 5. Registry (`regedit.py`)
- INI-style config at `/Pulsar/Registry/registry.cfg` with dot-notation API
  (`reg get`/`reg set Section.Key`).
- In-memory cache (parse once, write-through, invalidate on change).
- Sections: `[Networks]`, `[Hardware]`, `[System]`, `[Settings]`, `[Features]`,
  `[Globals]`, plus app keys (e.g. `[Apps]`).
- Personalization: `System.Owner`, `System.TZ_Offset` (applied to `date`),
  `System.Device_ID` (prompt hostname), `System.Codename` (synced from
  `OS_CODENAME` at boot).

## 6. Users & security (`usrmgmt.py`)
- CSV user store `/Pulsar/Registry/user.cfg`: `username, salt$sha256, /home/`.
- **Salted SHA-256** hashing, unique per-user salt; backward-compatible with
  legacy bare-SHA256.
- NOPASS marker for the guest account; per-user home directories.
- Backup/restore safety around every write.

## 7. Networking (`net.py`)
- WiFi: detect, scan, connect, disconnect, unlimited saved networks
  (`/Pulsar/Registry/networks.cfg`).
- **`wifi autoconnect`** — scan saved networks and connect to the strongest;
  optional boot autoconnect (`Settings.Network_Autoconnect`).
- HTTP client v2 — fully iterative redirect following, 15 s timeout, streamed
  bodies (no whole-file RAM), HTTPS with heap-consolidation nudge for Pico 1 W.
- `wget` / `curl` / `run_url` / `ping` / `nslookup`.

## 8. Package manager (`pkgmgr.py` + `pkg` command)
apt-style manager over a remote repo.
- `pkg repo add|remove|list`, `pkg update`, `pkg search`, `pkg available`,
  `pkg install <name…>` (multiple names), `pkg install <file.pkg>` (local),
  `pkg uninstall`, `pkg upgrade`, `pkg list`, `pkg info`.
- Package format: ZIP renamed `.pkg`, built `ZIP_STORED` (no compression
  dependency on-device); optional compiled `.mpy` packages.
- Built-in packages: **PicoFetch** (`fetch`/`neofetch`), **RPCMark** (`bench`),
  **NTP** (`ntp sync`), plus Launchpad/Editor stubs. PicoFetch and NTP are
  removable; core stubs are protected.
- Repo packages: `calc`, `gpio`, `i2cscan`, `ntp`, `sysmon`, `ask`, `dht`,
  `speedtest`, `backup`, `helloworld`.

## 9. Settings TUI (`settings.py`)
- SysMon-styled borderless panel with efficient in-place single-row redraw.
- Toggles: `Verbose_Boot`, `Program_Execution`, `OC_On_Boot`, beeper,
  `SD_Support`, `Network_Autoconnect`.
- Editable fields: Owner, Timezone Offset, Device ID, Idle Logout — no
  `reg set` needed.

## 10. Apps
- **Text editor** (`editor.py`) — nano-style ANSI editor (`edit`/`nano`/`vi`);
  needs a real serial terminal.
- **PicoFetch** — neofetch-style system info (`fetch`).
- **RPCMark** — CPU/RAM benchmark (`bench`).
- **NTP** — `ntp sync [server]` / `status` / `server <host>`; sets the clock
  over UDP (handles MicroPython's 2000 epoch), local time via `System.TZ_Offset`.

## 11. Updates & recovery
- **OTA** — `update check` / `update online [--force]` against a `latest.json`
  manifest; streams the `.rpc` and applies it, preserving users/settings/packages.
- **`update from-file <path.rpc>`** — apply a staged update archive.
- **`factoryreset`** — soft reset (wipe users/registry/homes/non-builtin
  packages/logs; POST rebuilds the registry).
- **`reinstall [path.rpc]`** — full wipe + self-contained stub install.
- **`.rpc` format** — flat source-only ZIP (`.py`/`.cfg`/`.lp`), extracted on
  device via a minimal Central-Directory reader.
- **Web tooling** — Web Serial installer, OS-update-over-USB page, and a web
  package browser (`_xfer` base64 serial protocol — no REPL, no reboot).

## 12. Storage layout
```
/Pulsar/Registry/   registry.cfg, user.cfg, networks.cfg, aliases.cfg,
                    startup.cfg, tasks.cfg
/Pulsar/Logs/       latest.log (rotated, batched flushing)
/Pulsar/pkg/        repos.cfg, cache/
/Core/ /Packages/   OS + packages
/Users/<name>/      per-user home directories
```

---

*Targets: RP2040, RP2350, ESP32-S3 · MicroPython (not CPython) · ~264 KB RAM on
Pico 1 — command files stay small and use lazy imports.*
