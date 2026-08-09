# Testing RPCortex v2

One command runs everything that can be run without hardware:

```
cd os/host && ./run_all.sh
```

Close to sixty suites, about half a minute, and the last line is the answer.
Anything else in this file is detail for when that line is not `0 failed`.

`realapp_test` needs the packages built, so it fails in a tree that has not run
`build.sh` yet. Everything else stands alone.

---

## What is proven without a board

`os/core/` is deliberately free of hardware headers, which is what makes most of
the OS ordinary testable code. On top of that, `host/fakehw.cpp` implements the
package ABI, so packages — which only ever call `fw_*` — are testable too.

| Suite | Covers |
|---|---|
| `smp_test` | The scheduler on two real threads. Double-scheduling, sleep handover, affinity gating. |
| `task_test` | Spawn, exit, kill, slot reclamation, stack guards, and that the hardware guard follows the running task rather than the core. |
| `mpu_test` | Encoding a protection region on both architectures, and every request that has to be refused rather than rounded. |
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
probe          cores, timing, jitter, hardware, memory - paste the whole block
diag           version, uptime, storage, and whether the last run crashed
mpu            what the memory protection has configured, per core
logdump        the log ring, when something looks wrong
```

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

## Adding a test

Put the file in `host/`, add one line to the `SRC` map in `run_all.sh` naming
the extra sources it needs, and that is all — there is no build system here on
purpose, because a test that runs from one script is a test that gets run.

**Prove a new test can fail.** Every check in this suite was verified by
breaking the thing it covers and confirming it goes red. A test written against
code that already works, never seen to fail, is a test that asserts its own
assumptions. The commit history has the specific reintroductions used.
