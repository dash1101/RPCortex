# RPCortex v2 — status

Where the C++ rewrite stands. `ARCHITECTURE.md` is how it is built,
`VS-V1.md` is how it compares to the MicroPython release.

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
| **updates** | OTA over HTTPS, staged and verified, with a rollback slot | update |
| **memory protection** | hardware stack guard on every task; on RP2350 a package runs unprivileged, on a stack and a heap of its own, and every pointer it hands the firmware is range-checked against those | mpu |
| **transfer area** | a real FAT12 volume over USB, off by default; `download` opens it and takes what lands into /usb | usb download |
| **shell** | full line editing, tab completion, pipes, `&&` / `\|\|` / `;`, `>` / `>>`, quoting | help |

Roughly 50 commands plus ~30 aliases (`ll`, `dir`, `more`, `del`, `free`, `gc`,
`id`, `exit`, …) in a separate table, so a second spelling costs two pointers
rather than a registry slot and a help line.

An installed package's commands go live at boot; an app registers commands via
the ABI (the `greet` app demonstrates it). That is the package system the Nova
D1 will sit inside.

**Tests** — 46 host suites, all green under ASan/UBSan: `os/host/run_all.sh`.
One of them, `fatimage_test`, is not a test of this code against its author's
reading of a specification — it synthesises a whole FAT volume and hands it to
`fsck.fat`, because a filesystem this device only ever writes and never reads
back has no failure that shows up on the device at all.
Every check is confirmed to fail when the thing it covers is broken; `TESTING.md`
says what is proven there and, more usefully, what is not. Two checks run against
the built image rather than the source, because the bugs they cover are invisible
in both the source and the host tests: `check-flashsafe.py` (everything the
firmware-writer touches is already in RAM) and `check-stackswitch.py` (every
stack switch releases the stack limit before it moves SP, and privilege is only
ever dropped in the veneer-pool gate).

**Footprint** — 961 KB on RP2350, 1007 KB on RP2040, of a 1024 KB slot. The
RP2040 build is over it and not shippable as it stands. Most of
the jump from ~290 KB is the CYW43 firmware blob (225 KB) plus lwIP. Task #72
holds the notes for getting it back down; `./build.sh --no-dev-packages` already
takes 23 KB off by leaving out bench, probe and stress.

**Run on hardware** — yes, on a Pico 2 W and a Pico 2, and that is where the
interesting bugs came from. Three of them could not have been caught here at all:

  * writing SP below MSPLIM is itself a fault, so a context switch has to
    release the stack limit before it moves SP — there is no MSPLIM on a host
  * the instruction after `msr CONTROL` runs unprivileged, so it cannot be in
    flash — both directions across the privilege boundary end in the package's
    own veneer pool
  * `flash_safe_execute` refuses immediately if the other core never registered
    as a lockout victim, and only core 1 had — so every filesystem write from a
    background task failed, while everything the OS itself writes runs on core 0
    and always worked

`stress` passes 25 checks on device: two cores, kill-at-next-yield, a sleeping
task waking on time while the shell keeps running, three concurrent filesystem
writers with no corruption, and an allocator that never hands the same block to
two callers. `probe` measures what a host cannot — an unpinned task lives on
core 1 99% of the time and changes core once in 2000 yields.

## What that means

The OS boots, logs in, runs a shell that reads like Vela, joins a network,
installs packages over HTTPS from its own repository, updates itself, and runs
those packages in a hardware sandbox they cannot reach out of.

That is on RP2350. **The RP2040 boards build and link but cannot currently have
a filesystem at all** — the firmware reserve is 2 MB and those boards hold 2 MB
in total, so littlefs is left with zero blocks. It went unnoticed because every
hardware test this year has been on a Pico 2 W. #81 has the numbers and the
options; until it is settled the honest claim for an original Pico is v1.0.

This is the base the Nova D1 gets ported onto.

## Not done

- **Package sandboxing is RP2350 only.** ARMv6-M regions are power-of-two sized
  and aligned to their own size, so the five a package needs would cost more RAM
  than an RP2040 has. There it runs packages privileged, as every build did
  before, and `mpu` says which a board is doing. Stack guards work on both.
- **A stack and a heap are allocated per call into package code** — 3 KB and
  12 KB, taken and given back around every package command. That is churn on a
  device where fragmentation has been a hard-stop before, and holding the pair
  per task instead is the obvious answer if it shows. Notes in
  `os/UNPRIV-DESIGN.md`.
- **The supervisor call is measured but the figure has never been read.**
  `probe` times 20,000 calls against an empty loop and reports nanoseconds and
  cycles; nobody has run it on a board yet.
- **`meminfo`'s fragmentation figure is DEVICE-UNCONFIRMED.** `heap_free()`
  reports the arena minus live allocations, which is honest rather than a
  high-water mark, and `largest_block()` probes with real `malloc` calls — but
  the two have never been compared over a long uptime. If they disagree, a
  healthy device reports high fragmentation, which is exactly the "diagnostic
  that invents a problem" failure v1 hit.
- **Drag-and-drop file transfer** (#69) — **built, untested on hardware.** The
  device presents a 1 MB transfer area over USB alongside the console: a real
  FAT12 volume in its own flash region, which the host owns outright and may
  create, edit, rename and delete in. `usb get` and `usb put` move files between
  it and the filesystem. `usb off` withholds it.

  It replaced a view synthesised over littlefs, which was built, shipped and
  abandoned — the reasoning is in `os/USBMSC-DESIGN.md` and is worth reading
  before anyone proposes it again. In short: inferring what a raw sector write
  MEANT can only ever handle the cases it was taught, and honouring one meant
  reaching into littlefs from inside the USB stack, which deadlocked against
  the console.
- ~~The site oversells all of this (#73)~~ — done. The landing page now names
  v1.0 as the release to run and v2 as early alpha, and the two wrong hero
  statistics are corrected.
- **Missing v1 commands** — `watch`, `edit`/`nano`, `task`/`service`/`startup`,
  `alias`/`unalias` at runtime, `wget`/`curl`. `edit` is a TUI, not an afternoon.
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
