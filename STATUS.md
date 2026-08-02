# RPCortex v2 — status

Where the C++ rewrite stands. Plan: `../tools/PLAN-v2.0-cpp.md`.

## Built and verified (host-tested; builds clean on RP2350 + RP2040)

**The loader** (`loader-spike/`) — runtime ELF app loading, 3.3 KB flash / no
static RAM. The go/no-go, and it went.

**The OS** (`os/`) — boots, logs in, and runs a shell that behaves like RPCortex:

| Area | What | Commands |
|---|---|---|
| kernel | boot, logging, honest heap accounting, the always-on clock | — |
| **accounts** | salted SHA-256, roles, NOPASS guest, first-run setup + login | whoami users logout |
| **registry** | dot-notation KV, persisted to flash | reg |
| **filesystem** | working directory, path resolution | cd pwd ls cat mkdir rm mv cp touch tree |
| **text** | file processing | echo grep wc head tail find |
| **system** | overview and clock | ver mem uptime date sysinfo reboot clear |
| **packages** | install / remove / list, boot-load, unload | pkg run apps unload put |
| shell | command history (up/down), login prompt with cwd | — |

An installed package's commands go live at boot; an app registers commands via
the ABI (the `greet` app demonstrates it). That is the package system the Nova
D1 will sit inside.

**Tests** — 129 checks across seven host suites, all green:
core 49 · path 16 · pkgindex 16 · textcore 12 · history 12 · apps 12 · loader 12.
Every subsystem with real logic (auth, path `..`, index dedup, wc off-by-ones,
history wraparound, package lifecycle, ELF relocation) is covered.

**Footprint** — RP2350 ~126 KB flash / ~19 KB static RAM; RP2040 similar,
comfortably inside its 264 KB — the board v1 had to drop.

## What that means

Tiers 1 and 2 of the plan are done: feature-parity shell + accounts + filesystem
+ commands + a working package system. This is the base the Nova D1 gets ported
onto.

## Not done

- **Not run on hardware.** All host-tested, both `.uf2`s build, nothing flashed —
  the board still carries v1.0 + Nova D1 and a flash erases it. First hardware
  step: flash `os/build_pico2_w/rpcortex_v2.uf2`, create root at the prompt, then
  `put greet.app <len>` + `pkg install greet.app` (or `run greet.app`).
- **Pipes and `&&`/`||`** — the shell runs one command per line; chaining is the
  next shell feature (needs output capture between stages).
- **A few more commands** — sort, uniq, which, alias, watch, history-list.
- **Nova D1 in C++** — Tier 3, the big one.
- **Networking, OTA, USB HID/BadUSB, BT audio** — Tier 4, beyond parity.
- `date` pulls in newlib's `sscanf` (~30 KB); worth hand-parsing later.

## Building

```
export PICO_SDK_PATH=$PWD/sdk
cmake -B os/build_pico2_w -G Ninja -DPICO_BOARD=pico2_w os   # or pico_w
ninja -C os/build_pico2_w
```

Host tests: `os/host/*_test.cpp`, compiled with g++ (each header notes deps).
