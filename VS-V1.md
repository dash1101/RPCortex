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
| Boards | Pico 2 W, ESP32-S3 | Pico 2 W, Pico 2 (RP2040 builds, see below) |
| Filesystem | 512 KB | **2 MB** on RP2350, **none** on RP2040 |
| Package isolation | none | unprivileged, MPU-enforced |
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

**Memory that is not a lottery.** No collector, so no fragmentation of the kind
that broke HTTPS on v1 and needed a reserved block held from boot to work around.

**A package cannot take the device down.** This is the largest gain over v1 and
the one with no equivalent there at all. On RP2350 a package runs unprivileged
with five protection regions describing everything it may touch, every pointer
it hands the firmware is range-checked against those regions, and it has a stack
and a heap of its own. A bad pointer costs the command and names the package; a
package that stops responding has the call taken back at a timer interrupt and
the shell survives. Both are confirmed on hardware with `havoc`.

A package exhausting its own stack is contained too, which took two things and
is worth writing down because the first attempt reasoned its way to the wrong
answer. The fault handler stands on 4 KB of its own per core, because it has to
release the stack guard to run at all and would otherwise write past the bottom
of the stack that just ran out. And the guard is armed on the package's own
stack a little ABOVE the bottom — so the overflowing instruction is refused with
the stack pointer intact, and there is room left to take the exception. With the
guard at zero, which is how it was, `sub sp` is not a memory access and nothing
stops the stack pointer leaving the region entirely; the frame then lands in the
heap and there is nothing valid to redirect.

**The RP2040 does not fit yet.** v1.0 dropped the Pico 1 and 1 W because the
multitasking build did not fit in 264 KB of RAM, and without an interpreter it
does. FLASH is the problem instead: `RPC_FW_RESERVE` is 2 MB — a megabyte to run
from and a megabyte to stage an update in — and a Pico has 2 MB in total, so
`FS_SIZE` computes to zero. The image boots and the shell runs and nothing can
be saved. A smaller RP2040-specific reserve is the fix and is planned (#81).

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
| `sd` card support | a driver |
| ESP32-S3 | a port: core/ moves unchanged, the context switch, storage and network layers do not |
| A filesystem on RP2040 | a reserve sized for a 2 MB part (#81) |
| The rest of v1's packages | rewriting in C; four are done |

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

## Confirmed on hardware

Everything below has been run on a Pico 2 W. Listed because most of it spent a
long time in the section under this one, and knowing what has actually executed
is the point of this document.

Wireless (`wifi` join and scan, `ping`, `nslookup`, `ntp`), HTTPS through
`wget`, `curl` and the `websearch` and `speedtest` packages, the package manager
installing and loading real `.app` files, `httpd` serving both a directory
listing and a site root, the USB transfer drive in both directions, `rm` with
wildcards and recursion, the editor, and both halves of package containment
(`havoc fault`, `havoc spin` and `havoc stack`).

The HTTPS deadlock that made downloads larger than a few KB hang is fixed — the
receive window was smaller than one TLS record, so mbedtls could not decrypt a
partial record and lwIP would not acknowledge what mbedtls had not consumed.
Small responses were unaffected, which is why the package index worked and a
firmware image did not.

## Known unstable

- **`update` writes flash and is DEVICE-UNCONFIRMED.** Everything up to the
  write — manifest, download, checksum, size and board checks — uses the same
  code the package manager does, which now has run on a board, and fails safely.
  The write itself can only be proven by doing it, and `update from-file` on a
  locally built image is the way to try it with nothing depending on the network.
  This is the largest untested path in the system.
- **The RP2040 build is untested beyond compiling.** With no filesystem there is
  little to test, and what a board does with a zero-length littlefs is not known.
- **`bench` and `probe` have never been read.** `bench` compares against the
  matching `tools/bench.py` on a v1 device and `probe` times 20,000 supervisor
  calls against an empty loop. Both run; neither number has been looked at, so
  every performance claim here is an argument rather than a measurement.
- **The fault handler's stack is sized off ONE measurement.** 4 KB per core, of
  which a contained fault used 644 bytes. That is the cheap path — it records
  and returns. The FATAL path still prints a full register and region dump, and
  nobody has read that figure, so the headroom is only proven for the case that
  does not reset.
- **The `stress` MPU crash is not closed.** It has not reproduced since the
  region write-ordering fix, which is not the same as being fixed. Evidence in
  `os/STRESS-CRASH.md`.

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

Two more were fixed and reproduced on the host before any board could run a
package command at all. Both are now confirmed — packages install, load and run:

- The loader adding a Thumb bit the symbol already carried.
- Long package commands killed for working: neither `bench` nor `stress` yields,
  so nothing fed the watchdog once the command started. Liveness comes from the
  ABI entry points, which a working package calls constantly.

And since:

- Firmware writes issued from core 1 failed silently for months, because only
  core 1 had registered for the flash lockout.
- The preemption interrupt wrote a program counter into an alarm dispatcher's
  locals. An SDK alarm callback runs several frames below the exception, so `lr`
  is an ordinary return address and `mrs r0, msp` gives the callback's stack
  rather than the frame. It is a naked handler on the alarm's own IRQ vector now.
- A fault handler that printed on the stack it was about to resume on. `printf`
  reaches the stdio mutex, the alarm pool's spinlock and TinyUSB's device task,
  none of which can be serviced at fault priority — all on the faulting
  package's stack, whose remaining depth is the thing in question. The handler
  records and returns; task context prints.
- A package's stack limit left at zero for the whole time it ran, so a stack
  pointer could walk past the bottom of its region and into the heap before any
  write faulted.
