# RPCortex v2 — status

Where the C++ rewrite stands. Updated as it progresses; the plan it works
against is `../tools/PLAN-v2.0-cpp.md`.

## Built and verified (host-tested, builds on RP2350 + RP2040)

**The loader** (`loader-spike/`) — runtime ELF app loading. 3.3 KB flash, no
static RAM. The go/no-go, and it went. 29/29 host checks.

**The OS core** (`os/`):

| Piece | What | Test |
|---|---|---|
| kernel | boot, logging, honest heap accounting | — |
| command registry | built-ins and apps register into one table | os 12/12 |
| loader integration | `run` loads/relocates/executes an app | os 12/12 |
| **registry** | dot-notation KV on littlefs (regedit) | core 49/49 |
| **accounts** | salted SHA-256, roles, NOPASS guest (usrmgmt) | core 49/49 |
| SHA-256 | vendored, FIPS-vector verified | core 49/49 |
| **login** | first-run setup + login loop (initialization) | — |
| persistence | registry.cfg / users.cfg load at boot, save on change | — |
| **filesystem cmds** | cd pwd ls cat mkdir rm mv cp touch tree | path 16/16 |
| path resolver | pure `.`/`..`/absolute/relative, cannot escape root | path 16/16 |
| **package table** | resident apps; unload sweeps commands + frees image | apps 12/12 |

Commands today: help ver mem ls cat cd pwd mkdir rm mv cp touch tree run put
apps unload whoami users reg logout clear reboot.

Footprint: RP2350 ~176 KB flash / ~19 KB static RAM; RP2040 ~95 KB text / ~17 KB
— comfortably within the 264 KB the RP2040 has, the board v1 had to drop.

## What that means

Tiers 1 and 2 of the plan are essentially done: a shell over serial that behaves
like RPCortex, with accounts, a filesystem, the commands, and a package system
that an app plugs into by registering commands (the `greet` app demonstrates it).
This is the feature-parity core the Nova D1 will sit inside.

## Not done

- **Not run on hardware.** Everything is host-tested and both firmwares build,
  but nothing has been flashed — the only board is running v1.0 + Nova D1, and a
  flash erases it. First hardware step: flash `os/build/rpcortex_v2.uf2`, watch
  it boot to a login prompt, create root, run `greet`.
- **Package manager** (install/remove/list over an index) — the table it needs
  exists; this is next.
- **Nova D1 in C++** — Tier 3, the big one.
- **Networking, OTA, USB HID/BadUSB, BT audio** — Tier 4, beyond parity.
- A few more system commands (uptime, date, watch) — minor parity gaps.

## Building it

```
export PICO_SDK_PATH=$PWD/sdk
cmake -B os/build -G Ninja -DPICO_BOARD=pico2_w os      # or pico_w for RP2040
ninja -C os/build
```

Host tests (no hardware): the `os/host/*_test.cpp` files, compiled with g++ —
see each file's header for its command line.
