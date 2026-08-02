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
| **wireless** | cyw43 + lwIP, saved networks, autoconnect | wifi scan/connect/disconnect/list/forget/auto |
| **packages** | install / remove / list, boot-load, unload | pkg run apps unload put |
| **shell** | history recall, pipes, `&&` / `\|\|` / `;`, `>` / `>>`, quoting | help |

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

**Footprint** — 290 KB `.uf2` on RP2350, 301 KB on RP2040; both comfortably
inside their flash, and the RP2040 is the board v1 had to drop.

## What that means

Tiers 1 and 2 of the plan are done, plus the console parity and wireless that
make it feel like the same OS. This is the base the Nova D1 gets ported onto.

## Not done

- **Not run on hardware.** All host-tested, both `.uf2`s build, nothing flashed.
  First hardware step: BOOTSEL, copy `out/rpcortex-v2-pico2_w.uf2` onto the
  RPI-RP2 drive, walk the first-run prompts, then `put greet.app <len>` +
  `pkg install greet.app`.
- **Wireless is unproven on hardware.** It compiles and links against the real
  cyw43 driver, but no scan has ever run — treat `wifi` as DEVICE-UNCONFIRMED
  until a board says otherwise.
- **No OTA / update command.** Flashing is drag-and-drop `.uf2` for now.
- **Missing v1 commands** — `watch`, `edit`/`nano`, `script`, `task`/`service`/
  `startup`, `safeboot`, `diag`/`fscheck`/`logdump`, `alias`/`unalias` at
  runtime, `ping`/`wget`/`curl`. The last group needs more lwIP surface; `edit`
  is a TUI, not an afternoon.
- **Nova D1 in C++** — Tier 3, the big one.
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
