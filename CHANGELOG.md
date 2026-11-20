# Changelog

Newest first. Notes for the next release are written under **Unreleased** as the work
happens: one `Summary:` line in plain words, then bullets under Added, Changed, Fixed and
Known limitations. `tools/release.py cut` turns that section into a dated entry, builds
the images, publishes the release, and updates the file the websites read. How to write
good notes is in `RELEASING.md`.

## Unreleased

Summary: The boot recovery ladder now works on hardware, and releases are numbered the new way.

### Added

- Versions are written `vMAJOR.ITER` with the build number beside them, with two digits for the iteration. Every release published takes the next number, and a label says how finished it is: `v2.00-beta` was the first, then `v2.01-beta`, release candidates as `-rc`, `-rc2` and so on, and no label once it is final. Only a new generation gets a name. `ver`, `update` and the rollback messages show the build.
- `tools/release.py`: one command to cut a release, and `tools/soak.py` to leave a board running and write down what it does.
- `releases/releases.json` and `releases/boards.json`: the release history and per-board status that the websites read.
- Services are supervised. A service that fails (a non-zero exit, which is also how a contained fault ends) is started again after 2 seconds, then 4, 8, 16 and 32, and given up on after five failures in a row; a minute of running normally clears the count. A service that was killed, finished cleanly, or ended without being seen is left alone. `service status` shows each one, and every restart and every give-up is in the log.
- Package API 1.25, `fw_time_set`: a package can set the clock. The Nova D1's GPS screen uses it to set the time from the satellites.
- Package API 1.26, `fw_net_scan2`: a Wi-Fi scan that keeps every radio. The older `fw_net_scan` merges beacons by name and drops networks with no name; this one reports one row per BSSID, hidden networks included, with the channel and the security the beacon declared. Wardrive on the Nova D1 uses it to write a WiGLE file.
- `subghz tx @<file>` sends the timings in a file, so a saved `.sub` can be replayed.
- Package API 1.27, `fw_nfc_select` and `fw_nfc_exchange`: a package can find a card on a PN532 reader and talk to it (InDataExchange), leaving the protocol to the package. The Nova D1's NFC app uses them to dump Mifare Classic and NTAG cards. Not yet run against a reader.

### Fixed (unreleased)

- Reading a file in pieces - how the rollback image is checked, how a WAV is played, and what a package's `fw_file_read_at` uses - entered littlefs without the filesystem lock, and so did the time stamp at the end of a streamed write. littlefs keeps one list of open files and one read cache for everyone, so a read on one task racing a write on another could leave that list pointing at freed memory. The likely cause of the filesystem that read 100% full until a big file was deleted (not yet confirmed on a board). All of them take the lock now, and a host check fails the build if a littlefs call is ever made outside it again.
- Saving the registry could write heap memory into `/os/registry.cfg`. The table was measured, then filled, then the fill's reported size written; a setting changed between the two (a package saving settings on the other core while the shell saved) made that size larger than the buffer. A save now measures again when the table grew, and writes nothing rather than a partial file if it will not hold still. A setting changed while the file was being written was also marked saved without being saved; the flag is now cleared before the snapshot, and saves run one at a time. The same applies to the accounts file and per-user settings.
- A package command looked up just before its package was unloaded (by another task, or a script running `unload` or `pkg install`) could be called anyway, directly and without its sandbox, into memory the package no longer owned. The lookup now copies the command out while the table is locked, and a package command whose package has gone is refused rather than called. Three loops over the command table also stop cleanly if it shrinks under them.
- A pipe or redirect inside a command run by a package (`fw_shell_run("ls | grep x")`) handed the next stage, or the file, the contents of an uninitialised buffer, and the package got nothing back. There was one output capture for the whole device: the inner pipeline could not start its own, then ended the package's. The same happened across tasks, when the shell ran a pipe while a package on the other core was capturing. Each capture now belongs to the task that started it and nests inside that task's previous one, only its owner can end it, and a pipe that cannot start a capture says so instead of running.
- Two rare cross-core races in the task table: reaping a finished task (`kill` on one) while a spawn on the other core reclaimed the same slot could free its stack twice and hand one slot to two tasks; and two cores spawning at once could give two tasks the same pid. Both now happen under the cross-core lock.
- A package reading a setting (`fw_reg_get`) copied it from the registry after the registry's lock had been released, so a setting changed or a user logging out at that moment could hand it half of one value and half of another, or someone else's. The copy is now taken under the lock.
- The shell's per-command state (the input of the current pipe stage, whether the command was run with `sudo`, and whether it printed an error) was shared by every task running commands. One task's command could clear another's error, so a chain like `cp x y && rm x` running beside a package's own command could remove `x` after the copy had failed. It is kept per task now, and a line run from inside a command on the same task no longer erases the outer command's error or input.
- A script (`.rps`) run with `bg` silenced the whole machine. The interpreter kept about 5 KB on the stack before running a line, and a background job had 3 KB to run a full shell line in. The interpreter's state is on the heap now (its frame is a few hundred bytes), and background jobs, services and the scheduler get the same 8 KB stack as the shell, since they run the same lines.
- After first-run setup, `/home/root` and `/home/guest` did not exist until the next boot. Setup makes them now.
- A task started by a package could run on far less stack than its firmware calls need. On RP2040 it got only what the package asked for, without the 8 KB firmware reserve every ABI call runs on; on RP2350 a package without veneer gates got 2 KB. It now gets the package's request plus the reserve whenever it will not be sandboxed.
- The memory cost of the stack changes above: each `bg` job, service and the scheduler now holds 8 KB instead of 3 KB (+5 KB each; eight services is 64 KB), and an unsandboxed package task holds its request plus 8 KB. Spawning fails cleanly when the heap is short, and says so.
- The RP2040 images (Pico, Pico W) stopped after POST: the stack guard's no-access window sat on the tripwire the scheduler reads at every task switch, and it was encoded as a 32-byte region, below the 256-byte minimum that MPU allows. The window is now one subregion of a valid 256-byte region and the scheduler leaves it to the hardware. Boot reaches the first-run setup in the emulator; real RP2040 boards are still to be tried.
- Typing at the console sends back what was typed, not the whole prompt and line again for every key: 130 bytes for a 48-character command where it was 3,604. It matters most on a UART console, and for pasted scripts, which used to come back as tens of kilobytes.
- `wc` takes `-l`, `-w` and `-c` (it used to report "No such file: -l"), counts several files with a total, and counts a named file even when a pipe is also feeding it. `capture n wc -l file` in a script now gives a number.
- A freshly flashed Pico 2 W no longer opens its log with "Last restart was the WATCHDOG - something stopped responding". The boot ROM restarts the chip through the watchdog after a flash; that restart is now told apart and noted as what it is.
- Boot is about a second faster when no terminal is open (after a reboot, or on battery). It waited up to three seconds for one; it now waits for the computer to recognise the device, then briefly for a terminal.
- `download` ended on the first key only sometimes: most keys were taken by its own Ctrl+C check and replayed at the next prompt (a `q` came back as `qls`). Commands and full-screen apps that read keys themselves now read them the same way the prompt does, so none go missing.
- `compat` and `pkg certs` froze a Pico 2 W whose radio had not been switched on, until the watchdog restarted it. Checking the certificates needs the network stack, which starts with the radio; with the radio off they now say so instead.
- `compat` warned "0 bytes free on the boot stack" on every board. It was measuring the wrong stack; it now reports the boot stack's real deepest point.
- A file or directory name of 40 characters or more was silently dropped from a path, so the path meant its parent instead: `touch /tmp/<long name>` made a file called `/tmp`, and `rm -r /home/root/<long name>` would have removed the whole home directory. Long names are kept whole now, and a path too long to handle is refused with "Path too long".
- `rm`, `mv` and `rename` refuse `/` and the system directories (`/os`, `/os/pkg`, `/etc`, `/home`, `/tmp`, `/usb`) themselves, even for root, and say how to empty one instead. `rm /tmp` used to delete it silently and break everything that writes there until a reboot. What is inside them can still be removed.
- `mkdir` now asks the same permission question as every other command that writes.
- `kill 1` said "It is the shell"; pid 1 is init, and the shell is told apart by what it is rather than by number.
- Usage messages, and `logdump`'s own "lines dropped" note, no longer go into the 48-line log, where a few of them pushed out the boot and crash entries it is there to keep.
- Tab completion of a path kept only the name: `ls /ho` became `ls home/`, a different path. The directory typed is kept now (`ls /home/`, `ls ../os/`).
- 14 KB more free memory on every board: buffers that were reserved for the whole time the device runs, but used only by one command (the boot scan, `download`, the USB drive's import, the task and service lists, Tab's candidate list), are taken when that command runs instead.
- Several pieces of shared state could be used by two tasks at once: the package loader's working buffers, `pkg install`, the task and service list files, the scheduler's table, and the `download` drive. Two `pkg install`s or two `download`s at once are now refused with a message; the rest take turns.
- `rm -r` could not remove a directory holding a name of 64 characters or more, and two `rm -r`s at once (one in the background) shared one working path. A factory reset had the same name limit. All three keep names whole now and work independently.
- `/tmp` is really emptied at every boot. Folders inside it were skipped and stayed for ever.
- A file standing where a system directory belongs is moved aside (to `<name>.displaced`) so the directory can be made again, instead of failing every boot.
- `df` no longer reports a failed measurement as a full disk. When littlefs cannot say how full it is, `df` says so and suggests `fscheck`, rather than printing "100% used" with no warning beside it.
- Two tasks could be inside I2C transfers on the same block at once (the display flush and a PN532 exchange), which the SDK's transfer calls do not allow. Every transfer, and every change of bus speed, now takes a per-block lock.

### Changed

- The update channel compares `MAJOR.ITER.BUILD`, for example `2.01.486`. An older device that compares `2.0.0.483` still updates into it.

### Fixed

- The boot recovery ladder (a maintenance start after three failed starts, then the saved firmware, then a rebuild) could never run on a board: the failure count was kept in a register the SDK overwrites before it is read. It is kept in one nothing else touches, and has been confirmed on a Pico 2 W along with `update rollback` in both directions.
- A build this device rolled back from is now actually refused by `update check`; the marker it kept never matched the version in the manifest.

### Known limitations

- Only the Pico 2 W has run this build. The Pico 2, Pico W and Pico images build and pass the image checks and have never been booted.
- A saved rollback copy takes most of a Pico 2 W's filesystem; the next update removes it to make room.

## v2.00-beta — "Vela II" (build 483, 2026-10-28)

The rewrite. RPCortex was MicroPython for four versions; this one is C++, and
almost everything underneath is different even though the shell is deliberately
familiar.

It is **in beta**. It boots, joins a network, installs and runs packages, serves
web pages, and updates itself over the air — all confirmed on real hardware. It
is also changing week to week, and the list of what is missing is at the bottom
rather than buried.

This entry is the narrative for the first release of the generation, v2.00-beta, which
was published on GitHub on 2026-10-28 as the tag `v2.0.0`. What changed in each later
release is in the dated sections above it.

---

### The headline

**No interpreter.** At 230 MHz, the clock v1's score of 5,000–6,000 was taken at (v1's
boot-clock setting was not reliable, so it was set by hand), a Pico 2 W scores about
**1.05 million** on RPCMark: roughly 190 times as fast, on the same workload at the same
clock. That is not a typo and it is not a projection — `bench` runs the same workload as
`tools/bench.py` does on v1, same iteration counts, so the two numbers are the same
measurement taken twice. At the 150 MHz v2 starts at it scores about 700,000, and the score
follows the clock almost exactly: 900,000 at 200 MHz, 1.1 million at 250 MHz (`pulse set`,
or `pulse boot` to keep it).

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
  doing when it stopped. v1 lost everything on reboot. After a bad shutdown the
  next boot also writes the black-box record and the log to `/os/crash.log`, so a
  crash survives the power being pulled as well.
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
- **A package can run from flash rather than from RAM.** The read-only half of a
  position-independent package — code, constants and the veneers into the
  firmware — holds no absolute address, so it is written to a flash slot once and
  executed where it lies. Only the writable half is resident: Nova D1 goes from
  174 KB to 62 KB. RP2350 boards only, and the parts of it a host cannot reach —
  the chip, fetching instructions out of XIP flash, and handing the memory
  protection unit a flash base — are listed as unconfirmed in `pkgslot.h` and
  have not been answered by a board. A slot reaches the firmware by index, so a
  firmware update that reordered or dropped an ABI symbol would leave those
  indices naming the wrong functions — the one corruption that does not fault, it
  just makes a package's own subcommands stop matching. The slot now records a
  fingerprint of the ABI table it was built against; a boot whose table no longer
  matches refuses the slot and loads that package from its `.app` instead, so an
  over-the-air update can never quietly break a slot-resident package. Append-only
  additions leave the fingerprint intact, so an ordinary update keeps the slot.
- **A Nova D1 app is one text file.** A `.napp` in `/nova/apps` is a short header
  and a dozen rows of `Label = shell command`. Nothing runs when one is
  installed, and nothing is executed afterwards either — the file is read. That
  is the whole security story, and it is also forced: one package call per task,
  and a Nova D1 screen is already inside one, so a screen cannot call into
  somebody else's package. What an app reaches instead is the firmware command
  surface — the radios, the contact readers, the network, the filesystem. What
  it cannot reach is the tools that happen to be packaged: `gpio`, `i2cscan`,
  `dht`, `ws2812`. A faulty app is listed with the reason it will not open
  rather than quietly dropped.
- **Protected files.** The registry, the user list, their backups and the device
  key are closed to every account except an admin: not readable, not changeable,
  and not reachable through a directory or a wildcard (`rm -r /os`, `rm /os/*`).
  That covers the shell commands, redirects, the editor, the USB drive and the
  file calls a package makes, so a guest cannot read saved secrets or edit the
  user list into making themselves an admin. v1 had no such protection.
- **Real background work.** v1's `task run` entered a foreground scheduler: it
  was scheduled tasks or an interactive shell, not both. Here the timer is just
  another task.
- **Interruptible everything.** Ctrl+C stops any command instantly, because the
  interrupt check and the scheduler yield are the same call.
- **The package loader refuses a damaged or hostile package instead of trusting it.**
  Fuzzing it found four faults, all in how it believed numbers read from the file: a
  relocation offset past the end of its section was a write wherever it pointed, and
  an odd one a misaligned read that faults on RP2040; a section-name offset past the
  end of the name table was a read from wherever it landed; and a section size or
  alignment that wrapped the 32-bit layout total allocated a small block and then
  filled it at the claimed size. Each is now a refusal with a reason. The packages
  `build.sh` produces load exactly as before.
- **Pastes and passwords arrive whole.** Only the shell's own task reads the
  console, so a background package polling for a stop request cannot take
  keystrokes meant for the prompt (it used to: pasted lines lost characters and a
  password typed at the login prompt arrived short). Anything typed while a
  command runs waits in a 512-byte stash, then in the USB buffer, which holds
  the rest back from the host when it fills. The login prompt and the line editor
  both read the stash first.
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
  computer. Confirmed on a Pico 2 W (`failboot 2` and `failboot 3` stand in for the
  failed starts): the third start loads nothing, and the fourth verifies the saved
  image, writes it back and comes up on the older build. Getting there found that
  the ladder had never been reachable: the start count lived in a watchdog scratch
  register that the SDK's own watchdog setup overwrites before the count is read,
  so every start counted as the first. It is kept in a register nothing else
  touches now. A restart that is not a failure (a hang the watchdog ended, a
  crash) counts once and is cleared when the shell comes up.

### What matches v1

The filesystem commands down to `ls`'s exact columns. Text processing. Accounts
with salted SHA-256, roles and the guided first run. The shell: pipes, `&&`,
`||`, `;`, redirection, quoting (single and double; a quote opens only at the start
of a word, so `wifi connect Dad's WiFi` and `echo don't` keep their apostrophes),
history, the aliases. `wifi` with all its
subcommands, `ping`, `nslookup`, `ntp`. `startup`, `task`, `service`, `watch`.
The `.rps` scripting language, unchanged — scripts carry over as they are. Idle auto-logout
(`Shell.Idle_Logout`, minutes, off by default). `curl` takes all of v1's flags: `-X`, `-d`, `-H`,
`-I`, `-v`, `-s`, `-o` and `--timeout`. `pulse boot <MHz>` sets the clock the device
starts at, and it does apply it: a start that follows one which did not finish skips
it and says so, so a clock that hangs the board cannot do it twice running.
`pulse boot off` turns it off.

And the console itself: v1's tagged output, colours and boot banner, checked
byte for byte against the original escape sequences.

---

### Boards

| Board | Image | Filesystem | Sandbox | Card |
|---|---|---|---|---|
| Pico 2 W | 855 KB | 1.75 MB | yes | yes |
| Pico 2 | 352 KB | 2.75 MB | yes | yes |
| Pico W | 805 KB | 384 KB | no | no |
| Pico | 297 KB | 1.25 MB | no | no |

The image figures are the shipped `.bin` sizes, and the development build is the
largest one gets, so it is what every flash slot is sized against; the table they come from is
in `os/CMakeLists.txt`, next to the slot sizes it justifies. "Card" is a microSD
slot, which is built for the RP2350 boards only.

The RP2350 images build the scheduler, the sandbox and the package-call path at
-O3, and everything that waits on a serial line, a radio or a flash write (the
shell commands, the Bluetooth, TLS and TCP/IP stacks, littlefs and the USB and
FAT code) at -Os. That took the Pico 2 W image from 1009 KB to 855 KB, leaving
169 KB of its 1024 KB slot free; `-DRPC_COLD_SIZE=OFF` builds the old way to
compare. The RP2040 images were already built at -Os.

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

- **HTTPS needs about 45 KB of free memory.** A handshake holds the session, the
  certificate chain and the key exchange in the heap. Measured on a Pico 2 W: 41 KB
  free failed and 47 KB worked. A board with Nova D1 and every developer package
  loaded used to boot with 41 KB; it now boots with 69 KB. A handshake that runs out
  says so and gives the numbers, and `unload` or a restart brings the room back.
- **A Ctrl+C behind a full stash of type-ahead waits.** Input that arrives while a
  command runs is kept, up to about 500 bytes beyond the USB buffer; past that the
  check stops reading, so a Ctrl+C typed after a very large paste is seen when the
  command ends. Nothing typed is dropped to make room.
- **A rollback copy takes most of the filesystem.** `update install` saves the running
  firmware before it applies the new one (seen on a Pico 2 W: "Rollback copy saved:
  v2.0.0 (854 KB)"), and that copy is a file of 854 KB on a 1792 KB filesystem. The next
  update over the air removes it to make room and saves the running one again.
  `update from-file` leaves the file it was given where it is, so there is not room for
  both, and it says so and carries on without a rollback copy. Restoring a saved copy
  (`update rollback --now`, and the automatic restore after failed starts) has been run on a
  Pico 2 W in both directions, 483 to 481 by hand and 486 to 481 by the ladder; the restored
  image is the one that was saved, checked against its recorded SHA-256 before it is written.
- **A filesystem that reads 100% full with a saved firmware copy on it.** Seen twice on a
  Pico 2 W, once after `update from-file` and once after an update over the air: `df`
  reported 0 KB free of 1792 KB with about 1.3 MB of files on it, every write was refused
  ("Could not write"), and `fscheck` found nothing wrong. Removing the large saved image
  (`rm /os/rollback.img`, or `rm` of the file given to `update from-file`) gave back
  1164 KB at once. It has not been reproduced since (75 hours of uptime on the next build
  and a second update over the air were normal), and the cause is not known. The updater
  now drops the saved copy and measures the room again when a download does not fit.
- **Saved Wi-Fi passwords are sealed, not unbreakable.** They are stored
  encrypted under a per-device key (`/os/device.key`, left out of backups), so a
  copy of the registry, a backup or a support dump no longer shows them, and an
  ordinary account cannot read the key or the registry (see "Protected files").
  The key is in the same flash as the passwords, so someone holding the whole
  device can still recover one: there is no secure element to put it in.
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
