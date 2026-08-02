# RPCortex v2 — status

Where the C++ rewrite stands. Plan: `../tools/PLAN-v2.0-cpp.md`.

## Built and verified (host-tested; builds clean on RP2350 + RP2040)

**The loader** (`loader-spike/`) — runtime ELF app loading, 3.3 KB flash / no
static RAM. The go/no-go, and it went.

**The OS** (`os/`) — boots, walks through first-run setup, logs in, and runs a
shell that reads like RPCortex Vela on the same terminal.

| Area | What | Commands |
|---|---|---|
| kernel | boot, logging, honest heap accounting, the always-on clock | — |
| **console** | v1's tagged output and colours, the gradient boot banner | — |
| **accounts** | salted SHA-256, roles, NOPASS guest, guided first run, login backoff | whoami users mkacct passwd usermod rmuser logout |
| **registry** | dot-notation KV, persisted to flash | reg env |
| **filesystem** | working directory, path resolution, usage | cd pwd ls cat mkdir rm mv cp rename touch tree df du |
| **text** | file processing, pipeable | echo grep wc head tail find sort uniq hex basename dirname |
| **system** | overview, memory, clock control | ver sysinfo meminfo uptime date pulse freeup which history sleep reboot sreboot clear |
| **wireless** | cyw43 + lwIP, saved networks, autoconnect | wifi scan/connect/add/autoconnect/list/forget/auto |
| **network** | DNS, ICMP, SNTP — none of them needs TLS | ping nslookup ntp |
| **packages** | install / remove / list, boot-load, unload | pkg run apps unload put |
| **shell** | full line editing, tab completion, pipes, `&&` / `\|\|` / `;`, `>` / `>>`, quoting | help |

Roughly 50 commands plus ~30 aliases (`ll`, `dir`, `more`, `del`, `free`, `gc`,
`id`, `exit`, …) in a separate table, so a second spelling costs two pointers
rather than a registry slot and a help line.

An installed package's commands go live at boot; an app registers commands via
the ABI (the `greet` app demonstrates it). That is the package system the Nova
D1 will sit inside.

**Tests** — 9 host suites, all green under ASan/UBSan: `os/host/run_all.sh`.
core · path · pkgindex · textcore · history · apps · loader · **out** (the
output layer's escape sequences, byte for byte against v1) · **cmdline** (41
checks on the pipeline/quote/redirect parser).

**Footprint** — 878 KB `.uf2` on RP2350, 901 KB on RP2040. The jump from ~290 KB
is the CYW43 firmware blob (225 KB) plus lwIP, which only started compiling in
once the board-detection bug was fixed. Both sit comfortably inside their flash
alongside the 512 KB filesystem, and the RP2040 is the board v1 had to drop.

## What that means

Tiers 1 and 2 of the plan are done, plus the console parity and wireless that
make it feel like the same OS. This is the base the Nova D1 gets ported onto.

## Not done

- **Not run on hardware.** All host-tested, both `.uf2`s build, nothing flashed.
  First hardware step: BOOTSEL, copy `out/rpcortex-v2-pico2_w.uf2` onto the
  RPI-RP2 drive, walk the first-run prompts, then `put greet.app <len>` +
  `pkg install greet.app`.
- **Wireless works** — confirmed on a Pico 2 W. The board-detection bug is fixed:
  PICO_CYW43_SUPPORTED is a CMake variable that never reaches the preprocessor,
  so the C++ side had been compiling the "no hardware" stub while CMake linked
  the whole driver.
- **ping / nslookup / ntp are DEVICE-UNCONFIRMED.** They build and the lwIP
  locking is right by construction, but none has been run against a real network.
- **`meminfo`'s fragmentation figure is DEVICE-UNCONFIRMED.** `heap_free()`
  reports the arena minus live allocations (`mallinfo().uordblks`), which is
  honest rather than a high-water mark, and `largest_block()` probes with real
  `malloc` calls — but the two have never been compared on a running board. If
  they disagree, a healthy device will report high fragmentation, which is
  exactly the "diagnostic that invents a problem" failure v1 hit and the reason
  its probe cap had to be raised. Check this against a fresh boot first.
- **No OTA / update command.** Flashing is drag-and-drop `.uf2` for now.
- **Missing v1 commands** — `watch`, `edit`/`nano`, `script`, `task`/`service`/
  `startup`, `safeboot`, `diag`/`fscheck`/`logdump`, `alias`/`unalias` at
  runtime, `wget`/`curl`. `edit` is a TUI, not an afternoon.
- **The package FETCHER is not built yet.** The repository side is done —
  `repo-v2/` in RPCortex-repo, with an index generated from each package's own
  header — but `pkg` cannot download from it. That needs an HTTPS client, since
  GitHub raw and rpc.novalabs.app are both TLS-only: mbedtls (the SDK vendors
  it; the submodule is not fetched) plus a decision about how much RAM one TLS
  session may hold. v1's entire contiguous-memory problem started exactly there,
  so it is worth doing deliberately rather than quickly.
- **Nova D1 in C++** — Tier 3, the big one.
- **A MicroPython port for running .py apps** — wanted, deferred. Worth knowing
  before it starts: embedding MicroPython puts its GC-managed heap alongside
  newlib's malloc arena, so `meminfo` would then be reporting one of two heaps
  and "free memory" stops having a single answer. That shapes the design, and it
  is obvious now in a way it will not be in three months.
- **USB HID / BadUSB / U2F** — Tier 4, beyond parity.
- `date` pulls in newlib's `sscanf` (~30 KB); worth hand-parsing later.

## Building

```
./build.sh                 # both boards -> out/, then the host tests
./build.sh pico2_w         # one board
./build.sh --clean         # wipe the build directories first
```

`build.sh` fetches the two SDK submodules wireless needs (`lib/cyw43-driver`,
`lib/lwip`) if they are missing — without them a W-board build fails deep inside
lwIP headers rather than saying what is wrong.

Host tests alone: `os/host/run_all.sh`.
