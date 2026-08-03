# v2 against v1.0 "Vela"

What the C++ build gains, what it matches, what it still lacks, and what it does
differently on purpose. Kept current as things land — the point is to be able to
answer "is this ready" without guessing.

Counts come from diffing the two command registries programmatically, not from
memory.

## The short version

|  | v1.0 Vela | v2.0 Vela II |
|---|---|---|
| Language | MicroPython | C++17, native |
| Commands | 113 names | 105 names |
| Shared with v1 | — | 82 |
| Multitasking | cooperative, one core | cooperative, **both cores** |
| Boards | Pico 2 W, ESP32-S3 | Pico 2 W, Pico W, **Pico 2, Pico** |
| Filesystem | 512 KB | **3 MB** (Pico 2 W) |
| Install | firmware + paste files | **one .uf2, drag and drop** |
| Packages | `.py`/`.mpy` archives | compiled `.app`, loaded at runtime |

## What is genuinely better

**No interpreter.** `bench` measures it against the matching `tools/bench.py` on
a v1 device — same workloads, same iteration counts, so the comparison is a
measurement rather than a claim.

**Both cores.** v1 could not use core 1 at all: MicroPython's heap and collector
are not safe across cores. v2 runs one task table with both cores taking from it.

**Interruptible everything.** Ctrl+C stops any command instantly — a sleep, a
ping, a directory walk — because the interrupt check and the scheduler yield are
the same call, so every place that can be interrupted is also a place that can
be rescheduled.

**Real background work.** v1's `task run` entered a FOREGROUND scheduler: it
offered scheduled tasks or an interactive shell, not both, and the documented
workaround was to boot into the scheduler and give up the prompt. Here the timer
is just another task.

**A task manager.** `ps` shows pid, state, core, stack used against allocated,
CPU time and the path each was started from. `kill` stops one. v1 had none of
this — a misbehaving background service could only be found by rebooting.

**Crash reporting that survives the crash.** A log ring and a black box in memory
the reset does not clear, so the next boot says what the last run was doing when
it stopped. v1 lost everything on reboot.

**A filesystem that repairs itself.** Missing directories are recreated at boot;
three failed boots rebuild the filesystem rather than needing another computer.

**The RP2040 is back.** v1.0 had to drop the Pico 1 and 1 W — the multitasking
build did not fit in 264 KB. Without an interpreter it does.

**Memory that is not a lottery.** No collector, so no fragmentation of the kind
that broke HTTPS on v1 and needed a reserved block held from boot to work around.

## What matches v1

Filesystem (`ls` with v1's exact columns, `cd` `cat` `cp` `mv` `rm` `rename`
`tree` `df` `du` `touch` `mkdir`), text processing (`grep` `wc` `head` `tail`
`find` `sort` `uniq` `hex` `basename` `dirname` `echo`), accounts (salted
SHA-256, roles, NOPASS guest, the guided first-run walk-through, login backoff),
the shell (pipes, `&&`/`||`, `;`, `>`/`>>`, quoting, history, ~30 aliases),
system info (`sysinfo` `meminfo` `uptime` `date` `ver` `pulse` `freeup` `env`
`reg`), wireless (`wifi` with v1's subcommands, `ping` `nslookup` `ntp`),
automation (`startup` `task` `service` `watch`), and the console itself — v1's
tagged output, colours and boot banner, checked byte for byte against the
original escape sequences.

## Still missing

| What | Needs |
|---|---|
| `pkg install <name>` from a repo, `search`, `info`, `upgrade` | TLS on top of the HTTP client below |
| `curl` `runurl` | the same |
| `edit` / `nano` / `vi` | a TUI layer |
| `settings` panel | the same TUI layer |
| `update` / `safeboot` (OTA) | A/B flash slots |
| `.rps` scripting | a small interpreter — v1's semantics are simple enough to match |
| `sd` card support | a driver |
| `diag` `compat` `inputstat` `regreset` `pkgdisable` `pkgenable` | nothing; just work |
| ESP32-S3 | a port: core/ moves unchanged, the context switch, storage and network layers do not |

## Different on purpose

**`exec` does not run source.** v1 compiled and ran a `.py` on the device. There
is no interpreter here, so `exec` runs a compiled `.app` and says so plainly when
handed source, rather than failing with "no such app".

**Packages are compiled.** Existing `.py` packages need rebuilding. Running
Python under v2 is wanted but not promised.

**Two permission levels, not a matrix.** Anything that changes the machine for
everyone is admin-only; `sudo` raises it for one command, and only for someone
who is already an admin.

**`rawrepl` is `bootloader`.** v1 dropped to the MicroPython REPL so a host tool
could reflash. The equivalent here is handing USB back to the boot ROM.

## Known unstable

- **`wifi`, `ping`, `ntp` are unproven on hardware.** They build and the lwIP
  locking is right by construction, but none has been run against a real network.
- **`wget` is new and DEVICE-UNCONFIRMED.** The half that decides what happens —
  redirects, size caps, truncation, a full filesystem — is host-tested against
  both a fake transport and a real `python3 -m http.server`. The lwIP socket
  layer under it has never run on hardware. It is deliberately the small half.

## Why no package command ever ran

Kept because it took several passes to find and the reasoning is worth not
repeating. Every package command hard-faulted the instant it was invoked —
`bench` and `stress` alike, though they share almost nothing — while *loading* a
package always worked. That split was the whole clue.

The loader added the ARM Thumb bit to function addresses itself. It should not
have: AAELF already carries it in `st_value`, so a function at the start of its
section shows as `st_value = 1` in `readelf`. Adding a second one produced a
pointer to *function+2* with bit 0 clear, and `blx` to an even address is
`INVSTATE` — an illegal state fault, before the first instruction of the command
ran. `app_main` escaped it because the entry point is resolved through symbol
lookup rather than a relocation, which is exactly why loading worked and running
never did.

The device named it in the end: `fault INVSTATE no Thumb bit cfsr=00020000`, and
the crash mapper put the PC at `bench+0x262` and `stress+0xafa` across six runs.
`os/host/realapp_test.cpp` now loads the real built `.app` files through the real
loader and checks every code pointer in the image for its Thumb bit, and every
veneer target too — veneers sit outside the image, so the first scan cannot see
them, and they are how every firmware call is made. Reverting the fix makes it
report those same two offsets on the host, so the fault is reproducible
off-device and cannot come back unnoticed.

Its fake firmware symbol table hands back ODD addresses, because that is what a
board does: `api.cpp` builds its table from `&fn`, and the address of a Thumb
function carries bit 0. The fake previously returned even addresses — a state no
device is ever in, and one that bypassed the arithmetic under test.

## Reliability work that has landed

Every one of these was a real failure on a real board, and each is listed because
the next port will hit the same ground.

- Flash writes racing core 1 — XIP is unavailable during an erase, so the other
  core stalled and the erase was left half done. Corrupted the filesystem.
- A finished task returning off the end of its stack into a trampoline with no
  return address.
- The watchdog fed by core 1's idle loop, masking a deadlocked core 0.
- Blocking input loops that slept instead of yielding, so nothing fed the
  watchdog while a human typed.
- A stall timer comparing against a timestamp from before the reset, wrapping to
  49 days.
- The shell running on the 2 KB boot stack instead of its own.
- Built-in packages installing once and never updating with the firmware, so
  several passes of debugging checkpoints never actually ran.

Two more are fixed and reproduced on the host, but **DEVICE-UNCONFIRMED** — no
board has run them, because the fault below stopped every package command before
either could be reached:

- The loader adding a Thumb bit the symbol already carried. Reverting the fix
  reproduces the exact offsets the board reported, so the diagnosis is solid;
  what is unproven is only that nothing *else* waits behind it.
- Long package commands killed for working: neither `bench` nor `stress` yields,
  so nothing fed the watchdog once the command started. Liveness now comes from
  the ABI entry points, which a working package calls constantly. This one has
  never been observed working, only reasoned about — the first clean `bench` run
  is what confirms it.
