# Testing RPCortex v2

One command runs everything that can be run without hardware:

```
cd os/host && ./run_all.sh
```

Around sixty suites, about half a minute, and the last line is the answer to one
question. **It is not the question "does the OS work".** What that line covers
and what it cannot is the last section of this file, and it is the part worth
reading first.

`realapp_test` loads the packages `build.sh` produces, so it fails in a tree
that has not built yet. It says so by name — which file is missing and which
command produces it — rather than checking whatever happens to be there.

---

## What is proven without a board

`os/core/` is deliberately free of hardware headers, which is what makes most of
the OS ordinary testable code. On top of that, `host/fakehw.cpp` implements the
package ABI, so packages — which only ever call `fw_*` — are testable too.

| Suite | Covers |
|---|---|
| `smp_test` | The scheduler on two real threads. Double-scheduling, sleep handover, affinity gating. |
| `task_test` | Spawn, exit, kill, slot reclamation, stack guards, and that the hardware guard follows the running task rather than the core. |
| `mpu_test` | Encoding a protection region on both architectures, every request that has to be refused rather than rounded, and what a real package span costs on each — the arithmetic behind leaving the package regions out of the RP2040 build. |
| `realapp_test` | Every real built package through the real loader, three ways: copied to RAM, copied to RAM sandboxed, and installed to a flash slot and loaded from it. Relocations, the GOT, the veneers, every branch by where it lands, and — since #103 — whether every code pointer it produced lands somewhere the hardware would let the CPU fetch from. Plus one synthetic object, for the relocation no compiler here emits. |
| `lock_test` | Recursion, hand-off, and that "busy" is per task rather than per core. |
| `packages_test` | dht, i2cscan, gpio and ws2812 against a fake device. |
| `calc_test` | The expression parser: precedence, associativity, errors. |
| `pio_test` | Instruction encoding, against the datasheet's tables. |
| `preempt_test`, `excframe_test` | The preemption policy and the exception frame. |
| `httpparse_test`, `httpfetch_test`, `repoindex_test`, `pkgindex_test` | HTTP, the package index, the installed list. |
| `tui_test`, `tuikey_test`, `tuilist_test`, `lineedit_test` | The screen layer and the line editor. |
| `rps_test` | The script interpreter. |
| `sdproto_test`, `fatro_test` | Bringing a card up, against a fake card written from the specification, and reading the filesystem on it, against volumes `fsck.fat` has approved of. |
| `radio_test`, `nfcframe_test`, `onewire_test` | Frequency and register arithmetic for the two SPI radios, and the frames and checksums the two contact readers carry. The bus transactions themselves need the chip. |
| `btadv_test`, `btname_test` | The advertising payload builders, byte for byte. |
| `novagui_test`, `novashots` | The Nova D1 runner with the hardware faked, and a pass that opens every screen and looks at the panel it produced. |
| `cacerts` | That the shipped roots parse under the device's mbedtls config. |

The rest cover the filesystem paths, formatting, the log ring and the black box.

### The fake device

`host/fakehw.h` models the SHAPE of the hardware: a GPIO remembers what was
written to it, an I2C bus has devices at addresses it was told about, and a pin
can be given a scripted waveform so a bit-banged protocol sees exactly what a
real part would send.

That is enough to test every DECISION a package makes. `packages_test` builds
the precise pulse train a DHT22 emits for a given temperature and humidity and
asserts the decoder recovers them — including a negative temperature and a
deliberately corrupted checksum. Finding that class of bug on hardware needs an
oscilloscope.

Two things learned building it, both worth keeping in mind when extending it:

- **A scripted pin's waveform starts when the package RELEASES the line**, not
  when the test sets it up. That is the protocol — a one-wire part stays quiet
  while the host drives the line.
- **A pin read costs 250 ns of fake time.** It has to cost something or a poll
  loop spins for ever, and it has to be much less than the shortest pulse or the
  error accumulates. At 1 µs per read the DHT decoder read four bytes perfectly
  and got the fifth wrong, which looked exactly like a decoder bug.

---

## What is NOT proven without a board

This is the honest list, and it is what `probe` exists for.

| Not covered | Why | How to check |
|---|---|---|
| Interrupt jitter | A host has no USB stack or radio interrupt | `probe`, JITTER section |
| Real core migration | The harness models a core as a thread, so a task cannot move between them | `probe`, CORES section |
| Memory barriers | x86 has a stronger memory model than Cortex-M33; the `dmb` in `task_switch.S` is there on the strength of the architecture | device only |
| PIO actually running | The fake records programs, it does not execute them | a strip, a scope, or `ws2812 proof` |
| SPI and I2C on real parts | The fake models the SDK's return values, not the silicon | the part itself |
| Flash writes, OTA | Needs the real flash controller | `update check` / `update install` |
| USB CDC console | The whole console; a host test writes to a buffer | any interactive use |
| Memory protection actually protecting | The host has no MPU. The encoding and the placement are tested; whether the silicon refuses the access is not | `mpu`, then `stress` — an overflow should now name the task instead of corrupting something |
| Which permission each package region gets | `TaskAppMem` carries spans and nothing else. `MPU_RO_EXEC` and `MPU_RW_NOEXEC` are chosen in `task_app_mem_apply` in `os/mpu_rp2.cpp`, which has hardware headers and does not compile on a host — so nothing here would notice one becoming the other | `mpu` on the board, which prints what each core really holds |
| Package regions on an RP2040 | There are none: `task_app_mem_apply` compiles them out on ARMv6-M. `realapp_test` reports its region checks as skipped there rather than passed | nothing to check — a package on a Pico runs under the default map by design |
| A microSD card | The protocol and the filesystem are covered; the electrical and timing layer is not, and nothing has been run with a card attached | `sd status` with no card, then `sd mount` — `sdcard_rp2.cpp` lists the order and what each failure means |
| Bluetooth beyond the payload builders | btstack needs the CYW43 | `bt scan`, `bt advertise`, `btaudio play` |

The last one deserves a sentence of its own, because it is the only part of this
OS where a total failure looks identical to success. A region that was never
programmed, or programmed on one core and not the other, behaves exactly like a
working one until the moment it was supposed to catch something. `mpu` prints
what each core actually has configured, which is the only way to tell from
outside.

Some of it can now be run with no board at all. `emu/` boots an image under
Renode on a community RP2040 model, which covers the scheduler on both cores,
littlefs on emulated flash, the loader, the package system and the shell. It
does not cover the sandbox, which is ARMv8-M only and compiles out on an RP2040,
and it has no radio and no USB. Output arrives; input does not. `emu/README.md`
has the limits and what it would take to lift them.

---

## On a device

```
tools/devsmoke.py   boot, version, install, run, render, memory, protection
tools/putfile.py    a file onto the device, acknowledged per chunk and verified
probe               cores, timing, jitter, hardware, memory - paste the whole block
diag                version, uptime, storage, and whether the last run crashed
mpu                 what the memory protection has configured, per core
logdump             the log ring, when something looks wrong
wifi                among other things, a count of unowned entries into the
                    radio driver - it prints that line only when it is not zero
havoc net <host>    the radio at rate from an unpinned task; run it as
                    'bg havoc net <host>' with 'wifi scan' against it
```

Two of those exist because a host test structurally cannot ask the question.
`havoc net` needs two real cores and a real radio driver, and the `wifi` count
needs the driver's own lock to be the core-keyed one the SDK ships — the
harness has neither.

`devsmoke.py` is the one to run first and the only one that answers by itself —
everything else prints numbers for a person to read. It takes `--port` and
leaves the radio exactly as it found it.

`probe` is the one that matters. It measures what the list above says cannot be
measured here, and it has found: an unpinned task living on one core 99% of the
time, a task table that ran out after four runs, and an I2C scan reporting every
address on an empty bus.

Package-specific checks that need nothing attached:

```
calc 2 ^ 3 ^ 2         512   (right-associative, and soft float works)
calc 100 / 5 / 2       10    (left-associative)
gpio list                    which pins the board allows
gpio temp                    a plausible die temperature
i2cscan                      "Nothing answered" with an empty bus
ws2812 info                  12 state machines on RP2350, 8 on RP2040
```

With hardware attached:

```
dht <pin> 22                 a DHT22 on that pin
i2cscan                      whatever is on the bus, named where known
ws2812 proof <pin> <count>   a strip, quiet then under load - they must look identical
```

---

## What a green run does not mean

Once, every suite passed while **every position-independent package on the
device was unable to execute a single instruction**. Not slow, not subtly
wrong — unable to run at all, every time, on every board. That is worth
keeping in front of anyone reading a green line, so here is exactly how it
happened and what it says about the shape of the hole.

### The worked example: #103

`app_pic_load` asked for a veneer gate pool of `VENEER_GATE_BYTES`, which is
48. ARMv8-M describes a protection region as a base and a limit on a 32-byte
granule, so `mpu_v8_encode` refused 48 bytes — correctly; rounding down would
leave the tail unprotected and rounding up would cover memory the package was
never given.

The refusal was silent. `set_region` programs **no region** rather than a wrong
one, which is the safe choice, and unprivileged code has no default map to fall
back on. No region means no access, including no instruction fetch. The first
instruction a sandboxed package executes after privilege is dropped is the
`bx r3` in the enter gate, which lives in that pool. It faulted there, every
time:

```
HARD FAULT in greet  pc=0x2006b1f8  lr=0x2006b201  cfsr=0x00000001
IACCVIOL not executable
```

Every number the loader produced was **right**. The entry point was right, the
GOT was right, every relocation resolved to the address it should have. The
suites checked all of that and had nothing to say about whether the CPU would
be allowed to fetch from it.

### The second worked example: the Thumb bit, again

#103 was found on a board. This one was found by writing the check that had
been missing, which is the better half of the same story.

`R_ARM_THM_MOVW_ABS_NC` and `THM_MOVT_ABS` build a 32-bit address in a register
out of two instruction halfwords, and it is the one relocation where the loader
supplies the Thumb bit **itself** rather than taking it from `st_value`. AAELF32
gives the result as `(S + A) | T` with `S` carrying no Thumb bit, and GNU ld
agrees: `movw`/`movt` against a Thumb function at `0x20001000` links to
`0x20001001`, the bit present exactly once. The loader's `S` already carried it,
so `S + T` added it a second time and built `function+2` with the bit **clear** —
the same value the `ABS32` case once produced, and the same INVSTATE fault at
the first call.

It survived because MOVW and MOVT are Thumb-2 instructions and every package
here is built for `cortex-m0plus`, which is ARMv6-M and has neither. Not one of
the six contains a single one of these relocations, so a check that walked the
real packages would have read zero sites, printed nothing, and passed for ever —
while the loader went on carrying the code for a pair an ARMv8-M build of a
package emits the moment its constants cannot live in the code section. That is the shape worth taking away: **a check with nothing
to check is not a check.** `realapp_test` therefore builds the smallest object
that does contain them — three pairs naming a Thumb function, a data object and
an undefined firmware symbol — loads it in both veneer modes, and prints how
many pairs it read from real packages (zero) beside how many from the fixture
(six). The data case is the control: `T` is 0 there, so it stayed correct
throughout, which is how the failure was known to be the Thumb bit and not the
field packing.

A fixture only ever contains what its author thought to put in it, so the walk
was checked against a real compiler too — and the first version of it was wrong
in exactly that way, pairing each MOVW with the MOVT four bytes on. That is true
of a hand-written pair and false of a scheduled one: in `greet` built for
Cortex-M33, the MOVW at `+0x14` has its MOVT at `+0x22` with two unrelated MOVWs
in between. It pairs by symbol and destination register now, nearest following
MOVT wins, each claimed once. The recipe for building that object is in
`realapp_test.cpp` beside the fixture; it holds five pairs, one of them the
address of a registered command, and the doubled Thumb bit fails on that one by
name.

### The four things a host cannot answer

The tables above list what individual features need a board for. This is the
shorter and more important list: the **categories** of question that no host
test can be written to answer, whatever effort goes into it.

**Execution.** The host has no ARM core. Every check here reads memory the
loader wrote and reasons about it; not one of them branches to it. A package
that loads perfectly and faults on its first instruction is, to `run_all.sh`, a
package that loaded perfectly. `realapp_test` narrows this as far as it can go
without a CPU — after relocation it walks the entry point, the three privilege
gates, every GOT slot naming a function and every ABS32 to a function, and
requires each to carry the Thumb bit and to land inside a span that will be
executable — but "would be allowed to fetch" is still a model, and a model of
a device is not the device.

Every kind of code pointer the loader produces is now read back, which took
three additions. Branch targets on the copy paths are checked by value, which is
what the slot path always did and the copy path never did — 6606 of them across
the six packages, in both veneer modes, each required either to reach the symbol
it names or to reach a veneer carrying the right destination. The sandboxed load
gets its GOT checked, which nothing did before. And the `MOVW`/`MOVT` pair is
decoded back out of the two halfwords it is built from, which found a real bug —
the second worked example below.

What is left is the limit itself, and no amount of this changes it: the
addresses are right and the map says the CPU would be allowed to fetch them.
Whether it does is the board's answer.

**Memory protection and W^X.** There is no MPU here. `mpu_test` proves the
encoding and `realapp_test` proves the loader's spans are encodable, that its
shadow of them keeps code and data apart, and that the shadow still matches the
`describe()` it is a shadow of. What stays out of reach is below; the last of it
is what a green line here does not cover on a board.

Whether the silicon actually refuses the access is the obvious one — an
unprogrammed region behaves exactly like a working one until the moment it was
supposed to catch something. `mpu` on the board prints what each core really
has.

**None of it applies on an RP2040 at all**, and that is now said out loud on
every run rather than left to this file. `task_app_mem_apply` compiles the
package regions out on ARMv6-M, so on a Pico or a Pico W a package runs under
the default map with no code, no data and no veneer region — not untested,
*absent*. `realapp_test` counts the region checks that mean nothing there and
prints them as **skipped, not passed**, with the reason measured beside them:
how many bytes of spans there are, what they would cost as power-of-two regions
— about twice as much, on the current packages — and how many are not
describable on that architecture at all. If they ever all become describable — because the loader's blocks changed, or because the encoder
started rounding — the grounds for compiling the regions out have gone and the
suite fails rather than going on quoting a reason that stopped being true.
`mpu_test` carries the same arithmetic for one real span: the read-only half of
`stress` is 4800 bytes and one exact ARMv8-M region; on ARMv6-M it is no region
at all, and giving it one costs an 8 KB region on an 8 KB boundary and up to
16 KB of heap to place it.

The shadow map is still a **copy** of `describe()` in `os/shell/apps.cpp`, on
purpose — a helper that called the loader's own placement would agree with it
whatever either of them did — but the two are now compared. `host_describe.cpp`
includes `apps.cpp` so the real `describe()` can be called, and its three spans
must match the shadow's on every path a package is loaded by. Editing
`describe()` to hand out a different span fails here instead of on a board;
dropping its round-up to a whole protection block reproduces #103 exactly, and
is caught by the spans disagreeing before the encodability check gets to it.

What that does **not** reach is the permissions. A `TaskAppMem` carries spans
and nothing else: `MPU_RO_EXEC` and `MPU_RW_NOEXEC` are chosen in
`task_app_mem_apply` in `os/mpu_rp2.cpp`, which has hardware headers and cannot
be compiled on a host at all. Nothing here would notice one of those three being
changed to the other.

The slot path used to be proved by one 304-byte package. A package reaches it
only by being position-independent, and `rpc_add_app(greet PIC)` was the only
opt-in — so every slot-path assertion (r9 against the GOT base, the three gates
being fetchable, each GOT slot landing in executable memory) was exercised by
greet's 15 GOT sites and nine branches, while the package the path exists for
sat on the copy path. The opt-in is Nova D1 now, and the counts are asserted
rather than the number of packages: the slot path must resolve at least 500 GOT
and 500 branch sites in a blob of at least 32 KB, and it prints what it actually
got — comfortably past all three today. Putting the opt-in back on greet
reproduces the old cover exactly and the suite says so instead of printing a
pass.

**Flash timing and watchdog margin.** `pkgslot_test` proves the slot format and
the write order against a fake chip that erases to 0xFF and refuses to turn a
bit back on, which covers every question about *what* is written and in what
order. It cannot cover *how long*: a real sector erase stalls the bus, the
other core cannot fetch from XIP while it happens, and whether that fits inside
the watchdog's patience is a property of the silicon and the clock. Nothing on
a host has a duration that means anything.

**Radios and the second core.** The harness models a core as a thread, so a
task never really migrates, and x86's memory model is stronger than a
Cortex-M33's — the `dmb` in `task_switch.S` is there on the strength of the
architecture manual, not of a test. Radio work is worse than untested: it is
*interactive* with the rest of the system. A status bar that reached into the
radio to draw a battery icon froze the whole board, and no arrangement of host
tests would have found it, because the fake radio answers instantly and the
real one does not.

### So what does a green run mean

That the arithmetic is right, that no case anybody has thought of regressed,
and that the shape of the code still matches the shape of the hardware. That is
a great deal, and it is why the suites exist. It is not the same claim as "a
board will run this", and the two have been confused exactly once, expensively.

The other end of it is one command:

```
tools/devsmoke.py
```

It reboots a board over serial, installs and runs a package, brings the Nova D1
screen up, photographs it, and reads the memory and protection reports back.
Twelve checks, about forty seconds, non-zero exit on any of them. The one that
matters most is the last: the fault handler runs on a stack of its own, so if
its high-water mark is still zero after a pass that dropped privilege and ran
package code, then nothing faulted. Its own docstring lists what it does not
cover — no hardware attached, no network, and no in-place reinstall of a
package the size of Nova D1.

---

## Adding a test

Put the file in `host/`, add one line to the `SRC` map in `run_all.sh` naming
the extra sources it needs, and that is all — there is no build system here on
purpose, because a test that runs from one script is a test that gets run.

One suite is coupled more widely than the rest and it is worth knowing which.
`realapp_test` links `host_describe.cpp`, which includes `os/shell/apps.cpp` so
the real `describe()` can be compared against the shadow of it — and that drags
in what `apps.cpp` refers to. If a change there reaches for something new, the
loader suite fails to link with `undefined reference to <name>`, which reads as
nothing at all. The fix is one line in the stub list at the top of
`host_describe.cpp`. They are written out rather than collected away with a
linker flag precisely so the failure names what is missing.

**Prove a new test can fail.** Every check in this suite was verified by
breaking the thing it covers and confirming it goes red. A test written against
code that already works, never seen to fail, is a test that asserts its own
assumptions. The commit history has the specific reintroductions used.
