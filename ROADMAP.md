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

## 🟡 v0.9.1 — next

Shell power + automation: turning RPCortex from an interactive shell into
something you can script and leave running.

**Summary of what's coming:** pipes, startup tasks, scheduled tasks, a first-cut
scripting language, expanded recovery tools, download progress bars, and extended
`curl` flags.

### Shell & scripting

- 🟡 **Shell pipes** — `cat f | grep ERROR`. This is the prerequisite for
  everything else in this section.

  **Implementation plan:** Every command currently prints directly to stdout.
  Pipes need a capture layer. Cleanest approach: add an optional
  `_output_buf = []` to the shell scope; when a pipe is active, commands `append`
  to it instead of printing; the next command in the chain receives it via a
  `_stdin` injection. Requires a small API shim in each command file — but most
  commands only have one or two print sites. Parser change: `_split_cmds()` gains
  a second pass to split on `|` after splitting on `;`, building a pipeline list.
  Each stage gets `execute_command` called with capture=True.

- 🟡 **`&&` / `||` chaining** — conditional sequencing based on exit code.
  Falls out of the same exit-code work as pipes. Each `execute_command` call
  returns `True`/`False`; `_split_cmds` records the operator between sub-commands.

- 🟡 **Scripting language (first cut)** — `.rps` files: variables (`$x = value`),
  `if`/`else`/`end`, `while`/`end`, `for x in list`/`end`, `~` expansion,
  and command dispatch through the live Launchpad engine.

  **Implementation plan:** a `Core/script.py` interpreter that reads the file
  line-by-line (no RAM-heavy AST), resolves `$vars`, handles the four block
  keywords via a stack of `{type, counter}` frames, and dispatches non-keyword
  lines to `execute_command`. Keep it under ~150 lines. Wire into `exec` command
  and the `execute_file()` fallback in launchpad for `.rps` extension.

### Task scheduling & background

- 🟡 **Startup tasks** — run commands/scripts/packages automatically after login.
  *Good first item — self-contained and low-risk.*

  **Implementation plan:** A `startup.cfg` file at `/Nebula/Registry/startup.cfg`
  (one command per line, `#` comments). `initialization.Startup_Process` (already
  called after login) reads the file and dispatches each line through the live
  `execute_command` reference. Shell management commands: `startup add <cmd>`,
  `startup remove <n>`, `startup list`, `startup clear`. The entire feature fits
  in ~50 lines across two files.

- 🟡 **Scheduled tasks** — interval execution using a software uptime offset
  (`utime.ticks_ms()`), since the RTC isn't battery-backed on bare Pico.

  **Implementation plan:** A `scheduler.cfg` at `/Nebula/Registry/scheduler.cfg`
  (`interval_secs:command` per line). The shell's main loop checks
  `utime.ticks_diff(now, last_tick) >= interval` after each command returns.
  No threading needed for a first cut. Shell commands: `cron add <secs> <cmd>`,
  `cron list`, `cron remove <n>`. Depends on the shell-loop having a defined
  "idle tick" concept — add that first.

- 🟡 **Unattended / background mode** — long-running tasks without holding the
  shell. A simple polling form is possible before uasyncio by running the task
  in a tight loop and checking for `KeyboardInterrupt` to yield. Full background
  support waits for uasyncio (v1.0).

### Recovery & hardware

- 🟡 **Expanded recovery tools** — diagnostics and repair inside `recovery_init()`.
  Currently recovery just gives you a stripped shell.

  **Plan:** Add a `recovery.lp` command file (separate from `system.lp` — keeps
  it loadable even with a corrupted `system.lp`) with:
  - `fscheck` — `uos.stat()` every path in a known-good manifest; report missing
    or zero-size files
  - `regreset` — delete `registry.cfg` so POST recreates it from template on next
    boot (does NOT touch `user.cfg` or `networks.cfg`)
  - `logdump` — print the full `/Nebula/Logs/latest.log`
  - `pkgdisable <name>` — rename a package dir to `<name>.disabled` so it can't
    be loaded, without removing it

- 🟡 **GPIO control via registry** — toggle pins from the shell to reset a hung
  peripheral without a reboot. `gpio set <pin> high|low`, `gpio read <pin>`,
  stored config via `Hardware.GPIO_<n>` keys.

### Networking & UX

- 🟡 **Download progress bars** — `[####----] 47%` for `wget`/`curl`/`pkg install`.

  **Plan:** `net._open_connection()` already parses headers; add a
  `Content-Length` check. If present, stream and print a `\r[####----] N%`
  overwrite line. If absent, print a spinner. Single helper `_progress(done, total)`
  in `net.py`.

- 🟡 **Extended `curl` flags** — `-X POST`, `-d '{"key":"val"}'`, `-H 'Auth: x'`,
  `-o /file`, `-s` (silent), `-I` (headers only), `--timeout N`.

  **Plan:** `sys_net.py curl()` currently just calls `net.curl(url)`. Wrap it with
  a lightweight arg parser (no `import re` — just `split(None, maxN)` scan) and
  extend `net.curl()` to accept `method`, `body`, `headers`, `output_file`,
  `silent`, `head_only`, `timeout` kwargs. ~80 lines total.

- 🟡 **OS personalisation** — owner name (`System.Owner`), timezone offset
  (`System.TZ_Offset`, applied to `date` output), device name already done via
  `System.Device_ID`.

### Memory & performance

- 🟡 Lazy / ephemeral imports — evict non-critical modules from `sys.modules`
  after use and run `gc.collect()`. Target: `net`, `pkgmgr` (rarely needed
  during a normal session).
- 🟡 Module eviction under memory pressure with automatic retry (build on the
  existing `MemoryError` auto-recovery in the shell loop).
- 🟡 Avoid string concatenation in hot paths — use `bytearray` or list-join in
  the log formatter and any loop that builds output incrementally.
- 🟡 Targeted Pico W RAM tuning — profile free RAM after a cold boot vs. after
  loading commands; aim for ~200 KB free ceiling.

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
- ⚪ **Extended benchmark** — NebulaMark gains mem / ROM / SD throughput tests.
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
| OS personalisation (owner / timezone) | 🟡 Next | v0.9.1 — `System.Owner`, `System.TZ_Offset` |
| Startup tasks | 🟡 Next | v0.9.1 — feasible, self-contained |
| Scheduled tasks | 🟡 Next | v0.9.1 — software uptime timing |
| Unattended / background mode | 🟡 Next | v0.9.1 polling form; full version needs uasyncio |
| Recovery mode tools (expand) | 🟡 Next | v0.9.1 — `recovery.lp` |
| Custom shell scripting language | 🟡 Next (first cut) | v0.9.1, after pipes/exit-codes |
| Shell pipes / `&&` / `||` | 🟡 Next | v0.9.1 — needs exit-code convention |
| GPIO via registry | 🟡 Next | v0.9.1 |
| Download progress bars | 🟡 Next | v0.9.1 — `Content-Length` in `net.py` |
| Extended `curl` flags | 🟡 Next | v0.9.1 — `-X/-d/-H/-o/-s/-I/--timeout` |
| Lazy / ephemeral imports | 🟡 Next | v0.9.1 — evict `net`/`pkgmgr` after use |
| Multitasking (uasyncio) | ⚪ Future | v1.0 foundation |
| Multiple terminals / tabbing | ⚪ Future | needs multitasking |
| SSH access | ⚪ Future | needs uasyncio |
| TUI framework + redesign of `settings`/`edit` | ⚪ Future | v1.0-era |
| Extended benchmark (mem/ROM/SD) | ⚪ Future | NebulaMark add-on |
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
| `ntp` | `ntp sync` → set RTC from `pool.ntp.org` over UDP | Solves RTC-resets-on-power-loss; pairs with `date` + startup tasks. ~60 lines. **Best first package.** |
| `dht` | `dht read <pin>` → temp + humidity from DHT11/DHT22 | The most common Pico add-on sensor; tiny driver (~40 lines). |
| `i2cscan` | Scan I2C buses, print detected addresses with known-device names | First thing anyone does with a new sensor. Pure debug value. |
| `gpio` | `gpio set/read/pwm <pin>` from the shell | Overlaps with the planned built-in `gpio`; could be the package form, or fold into core. |
| `calc` | `calc "3*(4+2)/1.5"`, `calc hex 255`, `calc bin 42` | Quick math/unit/base conversions without leaving the prompt. |
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
