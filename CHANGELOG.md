# Changelog

## v2.0.0 — "Vela II"

The rewrite. RPCortex was MicroPython for four versions; this one is C++, and
almost everything underneath is different even though the shell is deliberately
familiar.

It is **in beta**. It boots, joins a network, installs and runs packages, serves
web pages, and updates itself over the air — all confirmed on real hardware. It
is also changing week to week, and the list of what is missing is at the bottom
rather than buried.

The version stays at v2.0.0 until it ships, so this one entry grows rather than
splitting into a ladder of releases. What moves between builds is a build
number, taken from the commit count so it cannot be forgotten and cannot
disagree with the image; `ver` prints it and the updater compares it as a fourth
component.

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

| The package… | The cost |
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
  of reach. `bt` scans, advertises and names what answers, on LE and Classic;
  `btaudio` is an A2DP source, reading a WAV off flash and playing it to a
  speaker. A2DP is four layers deep — L2CAP, AVDTP, A2DP, then SBC — which is
  why it sits in the firmware rather than in a package.
- **A runaway task is a killed task.** A task that holds a core past its 250 ms
  slice is taken off it by a timer. Scheduling stays cooperative by default, so
  the reasoning about shared state is unchanged; preemption is the safety net,
  not the normal path. On v1 a `while True:` was a reboot.
- **A memory card.** `sd` mounts one at `/sd`, and it is a second volume rather
  than a second set of commands — `ls`, `cp`, `tree` and a script all reach it
  through the same paths. Read-only FAT12/16/32, RP2350 boards only, and it has
  not yet been run against a real card.
- **The parts a handheld is made of.** `nfc` and `ibutton` for the two contact
  readers, `subghz` for a CC1101 and `lora` for an SX1276. They are firmware
  rather than packages because those buses are shared, and arbitration is not
  something one package can do on behalf of the rest of the system.
- **Settings that belong to a person.** `User.`-prefixed registry keys are
  per-account, so two people on one device do not overwrite each other.
- **A package can run a shell command** and read back what it printed
  (`fw_shell_run`) — but not from inside another package command, which used to
  overwrite the outer call's way home and is now refused.
- **Real background work.** v1's `task run` entered a foreground scheduler: it
  was scheduled tasks or an interactive shell, not both. Here the timer is just
  another task.
- **Interruptible everything.** Ctrl+C stops any command instantly, because the
  interrupt check and the scheduler yield are the same call.
- **PIO, SPI and I2C for packages**, which is what makes drivers possible
  without touching the firmware.
- **It puts itself back together.** Settings and accounts are written twice, so
  a file that stops reading is restored from the copy beside it rather than
  lost. Every file the OS owns is read at boot, because a bad flash block on
  littlefs does not return wrong data — it returns nothing at all — and one that
  cannot be read is removed and rebuilt. `fscheck --scan` reads the rest.
- **Firmware rollback.** An update saves the version it replaces, and `update
  rollback` writes it back. Files and settings are untouched; only the firmware
  changes.
- **A device that recovers on its own.** Three starts that never reach a shell
  and it tries the cheapest fix first: load nothing — no packages, services or
  startup items, which is what breaks a working device most often. Still
  failing, it restores the saved firmware. Only when there is nothing left to
  restore does it rebuild the filesystem, which is the step that actually costs
  something. v1 had none of this: a device that would not boot needed another
  computer.

### What matches v1

The filesystem commands down to `ls`'s exact columns. Text processing. Accounts
with salted SHA-256, roles and the guided first run. The shell: pipes, `&&`,
`||`, `;`, redirection, quoting, history, the aliases. `wifi` with all its
subcommands, `ping`, `nslookup`, `ntp`. `startup`, `task`, `service`, `watch`.
The `.rps` scripting language, unchanged — scripts carry over as they are.

And the console itself: v1's tagged output, colours and boot banner, checked
byte for byte against the original escape sequences.

---

### Boards

| Board | Image | Filesystem | Sandbox | Card |
|---|---|---|---|---|
| Pico 2 W | 916 KB | 2 MB | yes | yes |
| Pico 2 | 320 KB | 3 MB | yes | yes |
| Pico W | 748 KB | 384 KB | no | no |
| Pico | 247 KB | 1.25 MB | no | no |

The image figures are the development build, which is the largest one gets and
is therefore what every flash slot is sized against; the table they come from is
in `os/CMakeLists.txt`, next to the slot sizes it justifies. "Card" is a microSD
slot, which is built for the RP2350 boards only.

**The RP2040 images have not been booted.** They build, both image checks pass
on them, and the flash layout was verified by reading the constant back out of
the compiled firmware — but no Pico or Pico W has run one. Every hardware
result quoted above is from a Pico 2 W.

The RP2040 boards are back. v1.0 had to drop them because the multitasking
build did not fit in 264 KB of RAM; without an interpreter it does. They have no
package sandbox — ARMv6-M protection regions are power-of-two sized and aligned
to their own size, so the five a package needs cost more RAM than those boards
have — and packages there run with the OS's own privileges.

---

### Known limitations

- **No package sandbox on RP2040.** Not an omission — ARMv6-M protection regions
  are power-of-two sized and aligned to their own size, so the five a package
  needs cost more RAM than those boards have. Packages there run with the OS's
  own privileges. `mpu` says which a board is doing.
- **The RP2040 images have never been booted.** As above: they build and they
  fit, and that is all anyone can say for them.
- **The card driver has never seen a card.** The command sequence that brings
  one up is host-tested against a fake card written from the specification, and
  the filesystem against volumes `fsck.fat` approves of. The electrical and
  timing layer is unproven and the source says so.
- **A task inside a lock or a flash write cannot be taken off its core.**
  Everything else can, at 250 ms. The exception is the case where interrupting
  costs more than waiting.
- **The wireless driver's lock is keyed on the core, and the OS works around
  it rather than fixing it.** cyw43's own mutex treats two tasks on one core as
  the same owner and makes a task on the other core wait in a `__wfe` with no
  timeout. Everything that reaches the radio therefore goes through one
  operation lock which migrates the caller to the core the chip came up on —
  `os/shell/netown.h` states the rule and every entry checks it, with `wifi`
  reporting a count of any that got through without it. The count is zero. The
  underlying SDK behaviour is unchanged and a new call site added carelessly
  can still reintroduce it.
- **`put` needs a sender that waits.** The device acknowledges each 256 bytes
  once they are on flash; `tools/putfile.py` waits for that and verifies a
  sha256, and is the only sender worth using. Bytes pasted by hand still work
  and are still unverified.
- **Not every v1 package has been ported.** The ones that have are in
  `repo-v2/index.json`; the rest are transcription rather than design, but they
  are still the distance left to parity.
- **No ESP32-S3.** v1 runs there; this does not yet. The portable core moves
  unchanged, but the context switch, storage and network layers do not.
- **Beta.** If something does not match the device, the device is right.

---

### Upgrading from v1

v2 replaces the firmware entirely, so installing it removes MicroPython and
everything stored under it. Anything worth keeping has to come off first —
`download` on v1, or whatever route is already in use.

Scripts carry over. Packages do not: a v1 `.pkg` is Python source and there is
no Python here.
