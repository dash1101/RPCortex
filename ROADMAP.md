# RPCortex Roadmap

The public, prettier version of this lives at
[rpc.novalabs.app/roadmap.html](https://rpc.novalabs.app/roadmap.html). This file
is the working dev roadmap — it tracks what shipped, what's next, and where loose
ideas landed.

Legend: ✅ shipped · 🔵 current · 🟡 next · ⚪ future · ❌ not started

---

## ✅ v0.8.1 "Nebula" — shipped (2026-04-02)

Final stable release of the β8 line. Highlights: unlimited saved WiFi, manual
`recovery` command, `--help`/`-h` on every command, `pkg commands`, tilde
expansion everywhere, password masking, functional `Program_Execution` toggle,
**echo file redirection** (`echo x > f` / `>> f`), POST `.mpy` fallback,
case-insensitive `cd`. See [CHANGELOG.md](CHANGELOG.md).

---

## 🔵 v0.9.0 "Pulsar" — current (2026-06-10)

First release of the β9 line and the **direct successor to v0.8.1** (the v0.8.2
work was internal and folded in here). Stability + quality-of-life.

- ✅ **Persistent aliases** — survive reboots (`/Nebula/Registry/aliases.cfg`)
- ✅ **Streamed `cp`/`mv`** — 1 KB chunks, fixes large-file OOM; `mv` same-FS rename fast-path
- ✅ **Relative paths** for `cp`/`mv`/`rename`; multi-file `cat`; `du`; `watch`
- ✅ **`date set`** — set the RTC so log timestamps are correct
- ✅ **Configurable prompt hostname** — `reg set System.Device_ID <name>`
- ✅ **OTA updates** — `update check` / `update online [--force]`
- ✅ **`fetch`/`bench` are packages** (PicoFetch / NebulaMark) — `pkg upgrade`-able
- ✅ **`pkg upgrade`** handles built-in packages + preserves registry keys
- ✅ **Version + codename never drift** — synced from running code each boot
- ✅ **Perf** — zero-I/O keystroke completion (no typing lag), batched log flushes (no output lag)
- ✅ **Portable `.mpy` build** — architecture-neutral, ~44% smaller, faster import

**Deferred out of v0.9.0 (on purpose):** shell pipes and `&&`/`||` — they need a
real per-command exit-code / stdout-capture convention first. They lead v0.9.1.

---

## 📊 Retrospective — Pulsar vs. the original "v0.9.0" plan

The first draft of v0.9.0 (archived in early planning notes) was an ambitious
*major* release: scripting language, pipes, scheduling, the first uasyncio pass,
TUI framework, SSH, deep memory work. **That plan was rescoped.** Pulsar shipped
as a stability + QoL release instead — lower risk, and it delivered a large batch
of items the original plan never mentioned. The big-ticket original items weren't
dropped; they were re-sequenced into v0.9.1 → v1.0 behind the foundations they
actually depend on (an exit-code convention for pipes; uasyncio for SSH /
multitasking).

**From the original plan, what landed in v0.9.0:**

| Original v0.9.0 item | Outcome |
|----------------------|---------|
| Tilde expansion everywhere | ✅ Shipped (actually v0.8.1-rc1) |
| Compile OS to `.mpy` + build script | ✅ Shipped |
| Chunked file I/O throughout | 🟡 Partial — `cp`/`mv` only; rest in v0.9.1 |
| OS personalisation (owner / device / timezone) | 🟡 Partial — device name done; owner + timezone in v0.9.1 |
| Module eviction on memory pressure + `gc` | 🟡 Partial — shell `MemoryError` auto-recovery clears cache + GCs; true eviction in v0.9.1 |
| Shell pipes · scripting · startup/scheduled/unattended tasks | ❌ → **v0.9.1** |
| Recovery tools · download progress · extended `curl` | ❌ → **v0.9.1** |
| uasyncio · TUI framework · SSH · `git`/`speedtest` pkgs · frozen modules · Windows stubs · extended benchmark | ❌ → **v1.0** |

**Shipped in v0.9.0 that the original plan never listed (bonus):** OTA updates,
persistent aliases, `watch`, `du`, `date set`, multi-file `cat`, streamed
`cp`/`mv` with relative paths, configurable prompt hostname, fetch/bench as
upgradeable packages, and the typing/output-lag fixes.

Net: ~2 of the original headline items fully shipped and ~3 partially, but ~11
*unplanned* improvements landed alongside. The original plan is now, effectively,
the v0.9.1 + v1.0 roadmap below.

---

## 🔵 v0.9.1 "Pulsar" — in development (shell power + automation)

Turning RPCortex from an interactive shell into something you can script and
leave running. The headline items build on a new per-command exit-code +
`multi()` output-capture convention in `RPCortex.py` (the piece deferred out of
v0.9.0). All items below are implemented and unit-tested in CPython; pending
on-hardware validation for the release tag.

### Shell & scripting

- ✅ **Shell pipes** — `cmd1 | cmd2 | cmd3`. `RPCortex.multi()` is the data
  channel: a capture buffer collects it per stage and feeds the next via
  `_shell_state['stdin']`; status helpers still print (stderr-like). Consumers
  `grep`/`wc`/`sort`/`uniq`/`cat`/`head`/`tail` read stdin when no file arg.
- ✅ **`&&` / `||` chaining** — exit status derived from whether a command called
  `error()`/`fatal()` (so no per-command changes were needed). `_run_line()`
  honours `;`, `&&`, `||`, and `|`; `_dispatch_line()` returns the status.
- ✅ **Scripting (`.rps`)** — `script <file>`: `set`/`$var`, `if`/`else`/`end`,
  `while`/`end`, builtins `eq`/`ne`/`exists`/`empty`, plus any shell command as a
  condition (exit status). `Core/Launchpad/sys_script.py`.

### Task scheduling & background

- ✅ **Startup tasks** — `/Nebula/Registry/startup.cfg` runs once after login via
  `launchpad._run_startup_tasks()`. Managed with `startup list|add|remove|clear|run`
  (`Core/Launchpad/sys_task.py`).
- ✅ **Scheduled tasks** — `task add <secs> <cmd>` (`/Nebula/Registry/tasks.cfg`);
  `task run` is a foreground scheduler firing due tasks on a `utime.ticks_ms`
  timer, staying responsive via `select()` (q/Ctrl+C to stop). Software-uptime
  timing — no battery RTC needed.
- ✅ **Unattended mode** — `startup add task run` boots the device straight into
  the scheduler loop (headless autonomy). True concurrent background tasks
  alongside an interactive prompt still wait for uasyncio (v1.0) — the shell's
  input read is blocking by design.

### Recovery & hardware

- ✅ **Expanded recovery tools** — `fscheck`, `diag`, `logdump`, `regreset`,
  `pkgdisable`/`pkgenable` in `Core/Launchpad/sys_recovery.py`, registered via
  their own `recovery.lp` (loads even if `system.lp` is damaged).
- ✅ **GPIO control** — shipped as the `gpio` package (`read|set|toggle|pwm|stop|adc`).
  Registry-backed *persistent* pin state (restore at boot via a startup task)
  is a possible follow-up.

### Networking & UX

- ✅ **Download progress bars** — `wget` prints `[####----] N%  done/total B`
  from `Content-Length` (redraws only on percent change; byte counter if size
  unknown). `net._draw_progress()`.
- ✅ **Extended `curl` flags** — `-X`, `-d`, `-H`, `-o`, `-s`, `-I`, `--timeout`;
  default GET-to-stdout (with redirects) unchanged. Quote-aware flag tokenizer in
  `sys_net.py`; richer request builder in `net.curl()`.
- ✅ **OS personalisation** — `System.Owner` (shown in `sysinfo`, prompted at
  first-run setup) and `System.TZ_Offset` (applied to `date`); device name via
  `System.Device_ID` was already done.

### Memory & performance (carry-over — still 🟡)

- 🟡 Lazy / ephemeral imports — evict `net`/`pkgmgr` from `sys.modules` after use.
- 🟡 Module eviction under memory pressure with automatic retry.
- 🟡 Avoid string concatenation in hot paths (fragmentation).
- 🟡 Targeted Pico W RAM tuning (~200 KB free ceiling).

---

## ⚪ v0.9.x → v1.0 — future

Tentative; most of this depends on the multitasking foundation landing first.

### Concurrency (the v1.0 milestone)
- ⚪ **First `uasyncio` integration** — cooperative multitasking for background tasks.
- ⚪ **Proper multi-threading & background services** — core architectural goal before v1.0; heavily tested.
- ⚪ **Multiple terminals & tabbing** — PowerShell-style; needs multitasking.
- ⚪ **SSH-style shell over TCP** — lightweight listener handing out a Launchpad session; needs uasyncio.

### Apps & developer tools
- ⚪ **TUI framework** — reusable adaptive box-draw components for terminal apps.
- ⚪ **Rebuild system apps on the TUI framework** — `settings`, `edit`, etc.
- ⚪ **Dev packages** — `git` (clone/fetch/push), `speedtest`, more (see Package ideas).
- ⚪ **Extended benchmark** — PulseMark gains mem / ROM / SD throughput tests.
- ⚪ **SD card support** — init package surfaced as a startup task.

### Build & portability
- ⚪ Frozen modules / custom `.uf2` firmware build (feasibility).
- ⚪ Windows MicroPython port stubs for off-device debugging (conditional `machine`
  imports, relative internal paths).
- ⚪ Mouse support in the TUI (terminal-dependent).
- ⚪ Viper / native optimisation — deferred; sacrifices non-RP2 portability.

---

## Idea audit — loose ideas mapped to status

This is where ad-hoc ideas get triaged so nothing gets lost or accidentally
re-implemented.

| Idea | Status | Where |
|------|--------|-------|
| echo output redirect to a file | ✅ **Already done** | `say()` in `sys_sys.py`, since v0.8.1 (`echo x > f` / `>> f`) |
| Improve execution speed | ✅ **Largely done**, ongoing | `__import__` loader (~95% faster, v0.8.1-b3), lag fixes (v0.9.0), `.mpy` image; more in v0.9.1 mem work |
| Chunked file operations | ✅ Done for `cp`/`mv` (v0.9.0) | extend to other I/O in v0.9.1 |
| Low-RAM warning | ✅ Done | shell warns < 70 KB (v0.8.1-b4) |
| Manual recovery entry | ✅ Done | `recovery` command (v0.8.1) |
| "Previous boot entered recovery" false warning | ✅ Fixed | boot sentinel handoff (v0.9.0) |
| Make `fetch`/`bench` packages | ✅ Done | PicoFetch / NebulaMark (v0.9.0) |
| Check-for-updates | ✅ Done | `update check` (v0.9.0) |
| Remove WiFi 2-connection limit | ✅ Done | `networks.cfg`, unlimited (v0.8.1) |
| Compile OS to `.mpy` + script | ✅ Done | `build_images.py`, `compile.bat` (v0.9.0) |
| Tilde expansion everywhere | ✅ Done | `_tilde_expand()` on all args (v0.8.1-rc1) |
| README rewrite | ✅ Done | (v0.9.0) |
| OS personalisation (device name) | ✅ Done | `System.Device_ID` (v0.9.0) |
| OS personalisation (owner / timezone) | ✅ Done | v0.9.1 — `System.Owner`, `System.TZ_Offset` |
| Startup tasks | ✅ Done | v0.9.1 — `startup` cmd + `startup.cfg` + engine hook |
| GPIO control | ✅ Done | v0.9.1 — shipped as the `gpio` package |
| Calc / I2C scan packages | ✅ Done | v0.9.1 — `calc`, `i2cscan` |
| Scheduled tasks | ✅ Done | v0.9.1 — `task` + `task run` scheduler (select-based) |
| Unattended / background mode | ✅ Done | v0.9.1 — `startup add task run`; true concurrency → uasyncio |
| Recovery mode tools (expand) | ✅ Done | v0.9.1 — `recovery.lp` + `sys_recovery.py` |
| Custom shell scripting language | ✅ Done | v0.9.1 — `.rps` (`sys_script.py`) |
| Shell pipes / `&&` / `||` | ✅ Done | v0.9.1 — capture + exit-code convention |
| Download progress bars | ✅ Done | v0.9.1 — `wget` via `Content-Length` |
| Extended `curl` flags | ✅ Done | v0.9.1 — `-X/-d/-H/-o/-s/-I/--timeout` |
| Lazy / ephemeral imports | 🟡 Next | carry-over to a later v0.9.x |
| Multitasking (uasyncio) | ⚪ Future | v1.0 foundation |
| Multiple terminals / tabbing | ⚪ Future | needs multitasking |
| SSH access | ⚪ Future | needs uasyncio |
| TUI framework + redesign of `settings`/`edit` | ⚪ Future | v1.0-era |
| Extended benchmark (mem/ROM/SD) | ⚪ Future | PulseMark add-on |
| SD card support | ⚪ Future | package + startup task |
| Candidate packages (`ntp`/`dht`/`mqtt`/…) | ⚪ Anytime | see Package ideas — don't gate releases |
| `git` package | ⚪ Future | dev tooling |
| Frozen modules / `.uf2` | ⚪ Future | feasibility |
| Windows debugging stubs / non-absolute paths | ⚪ Future | conditional imports |

---

## 📦 Package ideas — candidate add-ons

Packages ship independently of the OS, so these don't gate any release — they can
land whenever someone builds them. Roughly ordered by usefulness × ease. The OS
already has the networking and hardware primitives most of these need.

| Package | What it does | Notes |
|---------|--------------|-------|
| `calc` ✅ | `calc "3*(4+2)/1.5"`, `calc hex 255`, `calc bin 42` | **Shipped (v0.9.1).** Sandboxed eval (math only), base conversion. |
| `gpio` ✅ | `gpio read/set/toggle/pwm/stop/adc <pin>` | **Shipped (v0.9.1).** Live pin control on RP2/ESP32. |
| `i2cscan` ✅ | Scan the I2C bus, name detected devices | **Shipped (v0.9.1).** SoftI2C, any pins; common-device table. |
| `ntp` ✅ | `ntp sync` → set the clock from `pool.ntp.org` over UDP | **Shipped (v0.9.1), built-in + in repo.** Solves RTC-resets-on-power-loss; `startup add ntp sync` for boot sync. Handles MicroPython's 2000 epoch. |
| `dht` | `dht read <pin>` → temp + humidity from DHT11/DHT22 | The most common Pico add-on sensor; tiny driver (~40 lines). |
| `backup` | `backup create/restore <archive>` for home + registry | Safety net before `factoryreset` / `reinstall` / board migration. |
| `httpd` | `httpd start [port]` → live status page (CPU/RAM/uptime/temp) over WiFi | Opens remote-monitoring + basic REST use cases. Shell already has the data. |
| `mqtt` | `mqtt connect/pub/sub` | Makes RPCortex a real IoT node (Home Assistant / Node-RED speak MQTT). |
| `speedtest` | Measure up/down throughput, show Mbps/KBps | From the original roadmap; ships as a package so it updates independently. |
| `git` | `git clone` / `fetch` (read-mostly) | Dev tooling; heaviest of the set — needs careful RAM handling. |

---

## Design philosophy

- **Modular** — anything that can be a package, should be (fetch, bench already are).
- **Compilation is normal** — ship a portable `.mpy` image alongside source.
- **Comfort & personalisation** — the OS should feel like *yours*.
- **Don't ship half-features** — e.g. pipes wait for a real exit-code model rather than a fragile shim.
