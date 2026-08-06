# Changelog

## v2.0.0 — "Vela II"

The rewrite. RPCortex was MicroPython for four versions; this one is C++, and
almost everything underneath is different even though the shell is deliberately
familiar.

It is **early alpha**. It boots, joins a network, installs and runs packages,
serves web pages, and updates itself over the air — all confirmed on real
hardware. It is also changing week to week, and the list of what is missing is
at the bottom rather than buried.

---

### The headline

**No interpreter.** RPCMark scores around **1.2 million** against a v1 device's
5,000–6,000. That is not a typo and it is not a projection — `bench` runs the
same workload as `tools/bench.py` does on v1, same iteration counts, so the two
numbers are the same measurement taken twice.

**A bad package costs a command, not the device.** This is the part with no
equivalent in v1 at all. On RP2350 a package runs unprivileged, with the memory
protection unit describing exactly five regions it may touch, and every pointer
it hands the firmware range-checked against those regions. All three ways a
package can misbehave are contained and confirmed on hardware:

| The package… | Costs you |
|---|---|
| Uses a bad pointer | The command. A report names it; the shell carries on. |
| Stops responding entirely | The command. A timer takes the call back. |
| Runs out of its own stack | The command. |
| Asks for something that does not exist | Nothing. The call is refused. |

That last row sounds trivial and is not: `havoc` is a package built to attack
the ABI from inside the sandbox, and it runs 62 checks that all have to come
back as clean refusals.

**One image, drag and drop.** No firmware to flash and then files to paste on
top of it. Hold BOOTSEL, drop a `.uf2`, done — and after that the device
updates itself.

---

### What is new that v1 never had

- **Both cores.** v1 could not use core 1 at all; MicroPython's heap is not
  safe across cores. An unpinned task here lives on core 1 about 99% of the
  time.
- **A task manager.** `ps` shows pid, state, core, stack used against
  allocated, CPU time and where each task was started from. `kill` stops one.
  On v1 a misbehaving background service could only be found by rebooting.
- **Crash reporting that survives the crash.** A log ring and a black box in
  memory the reset does not clear, so the next boot says what the last run was
  doing when it stopped. v1 lost everything on reboot.
- **A USB drive.** `download` presents a real FAT volume, so files move by drag
  and drop in both directions instead of over the serial line.
- **Bluetooth.** The same chip does both radios. v1 could not reach Classic at
  all — MicroPython's module is BLE only — so anything that plays audio was out
  of reach.
- **Real background work.** v1's `task run` entered a foreground scheduler: you
  got scheduled tasks or an interactive shell, not both. Here the timer is just
  another task.
- **Interruptible everything.** Ctrl+C stops any command instantly, because the
  interrupt check and the scheduler yield are the same call.
- **PIO, SPI and I2C for packages**, which is what makes drivers possible
  without touching the firmware.

### What matches v1

The filesystem commands down to `ls`'s exact columns. Text processing. Accounts
with salted SHA-256, roles and the guided first run. The shell: pipes, `&&`,
`||`, `;`, redirection, quoting, history, thirty aliases. `wifi` with all its
subcommands, `ping`, `nslookup`, `ntp`. `startup`, `task`, `service`, `watch`.
The `.rps` scripting language, unchanged — scripts carry over as they are.

And the console itself: v1's tagged output, colours and boot banner, checked
byte for byte against the original escape sequences.

---

### Boards

| Board | Firmware | Filesystem | Sandbox |
|---|---|---|---|
| Pico 2 W | 916 KB | 2 MB | yes |
| Pico 2 | 320 KB | 3 MB | yes |
| Pico W | 748 KB | 384 KB | no |
| Pico | 247 KB | 1.25 MB | no |

The RP2040 boards are back. v1.0 had to drop them because the multitasking
build did not fit in 264 KB of RAM; without an interpreter it does. They have no
package sandbox — ARMv6-M protection regions are power-of-two sized and aligned
to their own size, so the five a package needs cost more RAM than those boards
have — and packages there run with the OS's own privileges.

---

### Known limitations

- **No Python.** Packages are compiled C or C++. That is the whole point of the
  rewrite and it is still a real loss of convenience.
- **Fourteen packages**, against v1's twenty. Porting the rest is the main
  distance left to parity.
- **No SD card support.** It needs a driver.
- **No ESP32-S3.** v1 runs there; this does not yet. The portable core moves
  unchanged, but the context switch, storage and network layers do not.
- **Cooperative scheduling.** A task that never yields cannot be killed from the
  shell. There is a timer backstop for packages, not for firmware.
- **Bluetooth is new.** Scanning works, both LE and Classic. Little else has
  been exercised.
- **Early alpha.** If something does not match the device, the device is right.

---

### Upgrading from v1

v2 replaces the firmware entirely, so installing it removes MicroPython and
everything stored under it. Save anything worth keeping first — `download` on
v1, or copy it off however you normally would.

Scripts carry over. Packages do not: a v1 `.pkg` is Python source and there is
no Python here.
