# Working on RPCortex v2 — notes for AI agents

This file is for an AI agent (or anyone new) about to change this repository. It
does not repeat the other documents; it says where they are, how to build and
prove a change, and, most importantly, the rules that are not visible from the
code you happen to be reading. **Almost every rule below is a bug that already
happened.** The comments in the code usually tell the story at the place it
happened; read them before changing that place.

Other documents, and when to read them:

| File | Read it when |
|---|---|
| `README.md` | First. What the OS is, building, layout. |
| `ARCHITECTURE.md` | Before changing the scheduler, the sandbox, networking or the package model. Includes "what should not be re-litigated". |
| `PACKAGES.md` | Before writing or changing a package, or the package ABI (`os/include/rpc_app.h`, `os/api.cpp`). |
| `TESTING.md` | To see what each host suite proves and what needs a board. |
| `RELEASING.md` | Before touching versions or `CHANGELOG.md`. |
| `LINK.md` | Before changing `link` (`os/shell/link.cpp`) or the host tools in `tools/rpc` and `tools/web`. |
| `PERMISSIONS.md` | Before adding a command, or changing who may run one, or the file guard. The table of what needs sudo is there. |
| `CHANGELOG.md` | Every change that a user would notice goes under `## Unreleased`. |

---

## 1. What this is

RPCortex v2 ("Vela II") is an operating system for Raspberry Pi RP2350 and RP2040
boards, written in C++17 on the Pico SDK. It has a shell with Unix-like commands,
user accounts, a littlefs filesystem in flash, a preemptive-on-stall cooperative
scheduler on both cores, packages (`.app` files) loaded at run time and, on
RP2350, run unprivileged under the MPU, Wi-Fi and HTTPS on the wireless boards,
Bluetooth, a USB drive mode, a script language (`.rps`), and over-the-air updates
with a recovery ladder.

The flagship package is **Nova D1**, a handheld multi-tool UI (`os/apps/novad1`).
The OS is a platform; Nova D1 is one thing built on it.

Boards (`./build.sh <board>`):

| Board | Chip | RAM | Firmware slot | Notes |
|---|---|---|---|---|
| `pico2_w` | RP2350 | 520 KB | 1024 KB | The main target. Wi-Fi + BT. Sandbox, flash slot for packages, MSPLIM stack guard. |
| `pico2` | RP2350 | 520 KB | 512 KB | No radio. |
| `pico_w` | RP2040 | 264 KB | 832 KB | Wi-Fi + BT. **Very little slot headroom**; check `build.sh` output. |
| `pico` | RP2040 | 264 KB | 384 KB | No radio. The emulator image is this board (`--emu`). |

RP2040 differences that matter: no sandbox (packages run privileged), no flash
slot for packages, the stack guard is a 32-byte MPU subregion (ARMv6-M's smallest
region is 256 bytes), less RAM. Real RP2040 silicon has not yet run v2.

---

## 2. Layout

```
os/core/        Pure logic. No hardware headers. Every file here is compiled by
                the host tests with ASan/UBSan. Put real logic HERE, behind a
                function-pointer seam if it needs hardware, so it can be tested.
os/kernel/      Boot (kboot), logging, heap accounting, version (kernel.h).
os/shell/       Commands, grouped by area (fs.cpp, text.cpp, net.cpp, pkg.cpp,
                jobs.cpp, link.cpp, ...). shell.cpp has the prompt loop, the line
                runner (run_line / run_segment) and command dispatch.
os/*.cpp        Platform glue: task_rp2.cpp (context switch, watchdog,
                preemption), mpu_rp2.cpp, sandbox_rp2.cpp, api.cpp (the package
                ABI: every fw_* function), main.cpp (boot order).
os/apps/        Packages built from this tree (greet is the smallest example).
os/include/     rpc_app.h, the header packages are written against.
os/patches/     Patches the build applies to COPIES of SDK files in the build
                directory, never to sdk/ itself (os/CMakeLists.txt says how).
os/uzlib/       The uzlib inflater, from MicroPython (zlib licence), for the
                radio firmware; tinflate.c carries one fix, marked as such.
os/host/        Host test suites. run_all.sh builds and runs them all.
loader-spike/firmware/
                storage.cpp (littlefs, the filesystem lock, sinks and sources),
                the package loader. Despite the directory name, this is live code.
emu/            Renode RP2040 emulator: run.sh, run-tests.sh, tests/*.robot.
tools/          Host tools: rpc (CLI), web (browser tool), putfile.py, devsmoke.py,
                soak.py, release.py, size and lock checkers.
sdk/ littlefs/  Vendored Pico SDK and littlefs. Do not edit.
```

---

## 3. Building and proving a change

```
./build.sh pico2_w            build one board AND run every host suite
./build.sh                    every board
./build.sh pico --emu         the emulator image (console on UART0)
os/host/run_all.sh            host suites only
emu/run-tests.sh [suite]      emulator suites (each takes 10 s to 3 min)
```

`build.sh` refuses an image that does not fit its slot, prints the headroom
(`N KB left in the M KB slot`), and warns (`tools/size-guard.py`) when pico2_w
grows more than 8 KB over `tools/size-baseline.json`. Note the headroom before and
after any change.

### Features: what an image is built with

```
./build.sh --features                       the features, their boards, the measured cost
./build.sh pico2_w --without bluetooth      a custom image: out/rpcortex-v2-pico2_w-nobt.uf2
./build.sh pico2_w --without sd,peripherals several at once
./build.sh pico --with devpkgs              add one (--dev-packages still works)
tools/buildconf.py                          the same by questions, then runs build.sh
tools/features.py measure                   rebuild every variant, rewrite tools/feature-sizes.json
                                            (--no-build: record the images already built)
```

The features are `wifi`, `bluetooth`, `ssh`, `sd`, `peripherals` (the NFC,
iButton, sub-GHz and LoRa drivers) and `devpkgs`. A board's **standard image** has
every feature it can have except SSH on the Pico W (its slot is too tight) and
`devpkgs`; that is what a release publishes.

- **The table is `tools/features.py`** (`FEATURES`): name, aliases, the tag an image
  name carries, the CMake option, the define, the boards that can have it, its
  default and what it requires. build.sh, os/CMakeLists.txt (at configure time,
  through `features.py cmake`, which writes `features.cmake` into the build
  directory), tools/buildconf.py and tools/release.py all read it. There is no
  second copy of a default anywhere.
- **A feature is whole files, never an #if in shared code.** Each has a source list
  in os/CMakeLists.txt (`RPC_WIFI_SOURCES`, `RPC_BLUETOOTH_SOURCES`,
  `RPC_SD_SOURCES`, `RPC_PERIPHERALS_SOURCES`, `RPC_SSH_SOURCES`) and its libraries
  are linked only with it. When it is off, a stand-in file is compiled instead:
  `net_off.cpp`, `bt_off.cpp`, `sd_off.cpp`, `periph_off.cpp`, and `radio_off.cpp`
  when neither radio is in. A stand-in registers the same command names, help
  lines, levels and aliases, has each answer through `feature_missing()`
  (os/shell/imagefeatures.h: "This image was built without Wi-Fi.", or "This board
  has no wireless hardware." where the board could never have it), and keeps every
  package call in the ABI table, failing the way it fails with no hardware. New
  code for a feature goes in that feature's files; the "no" answer goes in its
  stand-in.
- **Where a command is half network**, the network half is its own file: `pkg`
  (pkgrepo.cpp keeps the catalogue and `pkg install <file>`, pkgfetch.cpp
  downloads) and `update` (update.cpp keeps from-file, rollback and the boot
  report, which the recovery ladder needs; update_net.cpp checks and downloads).
  `pkg install <file>` must keep working without Wi-Fi: it is how tools/rpc and
  tools/web install a package on a board with no network.
- **The HTTP client is in every image; only its transport is Wi-Fi.** httpfetch,
  httpparse, pkgfetch.cpp, update_net.cpp and httpclient.cpp (wget, curl, runurl,
  `fw_http_*`) are built everywhere and ask `http_route()` (shell/httproute.h)
  which way a request goes: lwIP and TLS in httptransport.cpp with Wi-Fi, the
  network bridge in bridge.cpp without it. New code that fetches a URL goes
  through `http_route()` and `http_https_ready()`, never straight to a transport,
  or it works on one kind of image only.
- **The defines are always 0 or 1**, so they work in a plain `if`: `RPC_HAS_WIFI`,
  `RPC_HAS_BLUETOOTH`, `RPC_HAS_SSH`, `RPC_HAS_SD`, `RPC_HAS_PERIPHERALS`;
  `RPC_CAN_<X>` for whether the board could have it at all; and `RPC_STD_SSH` for
  whether its standard image has SSH, which is what decides whether ssh_off.cpp
  registers an `ssh` that explains itself (sd_off.cpp does the same with
  `RPC_CAN_SD`), so no standard image gains or loses a command. `RPC_HAS_CYW43` is a
  fact about the board, not a feature: GPIO 23/24/25/29 belong to the radio chip
  whatever the image carries, so `fw_gpio_*` keeps refusing them.
- **Bluetooth and SSH need Wi-Fi.** The CYW43 driver and its firmware come in
  with Wi-Fi, so an image without Wi-Fi has no radio at all: no driver, no
  firmware, no firmware streaming (`RPC_CYW43_FW_STREAM`, the patch step and
  cyw43_fw.cpp go too). radio.cpp owns the chip for both radios (the one-owner
  lock, `radio_up`/`radio_down`, the radio lock, `radio`); the Wi-Fi steps of
  bringing it up and down are the `wifi_radio_*` hooks in net.cpp. An image with
  Wi-Fi and without Bluetooth streams the plain firmware rather than the
  Bluetooth-capable one, and cyw43_fw.h checks the two agree. Nothing in the OS uses the W boards' LED, and VBUS is read through
  Wi-Fi's status cache, so on a W image without Wi-Fi `fw_power_source` answers
  unknown (as a full image does before the radio first comes up).
- **A custom image is named for how it differs**: `-no<tag>` for each feature left
  out (a feature that went only because what it needs went is not named again,
  so `-nowifi` covers Bluetooth and SSH), `-<tag>` for one added, `-emu` for the emulator image.
  It builds in its own directory (`os/build_pico2_w-nobt`) and is never compared
  against or recorded as the size baseline.
- **The image says what it is.** imagefeatures.cpp bakes `rpcimage:<variant>` into
  every image (`standard`, or the tag). build.sh checks it after every build
  against what it asked for; `ver` and `sysinfo` print the feature list and say
  "custom image"; tools/release.py reads the marker from the `.bin` and the `.uf2`
  and refuses anything that is not `standard`, whatever the file is called.
  `update install` on a custom image installs the standard one, and says so first.
- **The flash layout does not depend on the features.** `RPC_FW_RESERVE` and the
  package slot are per board, so a custom image gains slot headroom and RAM, never
  filesystem, and a board moves between a standard and a custom image keeping its
  files. Do not size the reserve from the features: the next update from an image
  with a smaller reserve would be written over the start of the filesystem.
- **build.sh passes every feature option on every configure.** A build directory
  remembers its cache, and an option left to the cache builds the last feature
  set rather than this one.
- **Adding a feature:** an entry in `FEATURES`; its option in
  `RPC_FEATURE_OPTIONS`, a source list and a stand-in in os/CMakeLists.txt; a
  `Feature` value and its message in imagefeatures; tests in
  tools/test_features.py; build every board with and without it; then
  `tools/features.py measure`. Only add one where switching it off removes real
  code and leaves a coherent OS.
- **SSH keeps its own option name, `RPC_SSH`** (the one the SSH work added), but
  its value comes from the table like every other feature's: on by default on the
  Pico 2 W only, and off whenever Wi-Fi is, since it runs over it. The sources are
  `RPC_SSH_SOURCES`; `RPC_HAS_SSH` is also read by mbedtls_config.h. sshd_boot
  stays under its `#if` in main.cpp; `ssh` itself is registered in every image,
  by sshc.cpp or ssh_off.cpp.
- **Not features, deliberately:** the USB drive (`download`) and the USB keyboard
  are interfaces of the static composite USB device (usb_descriptors.c, and
  usbmode.h keeps them idle). Taking one out would change the descriptor and the
  interface numbering for every image, for about 10 KB and 8 KB.
- **No header called `features.h`** under os/: the host suites put os/shell on
  the include path, and glibc's `<stdio.h>` includes its own `<features.h>`. One
  did, and every host suite failed to build.

**The order of proof:**

1. **Host test.** Put the logic in `os/core/` and test it in `os/host/`. Add the
   suite to the `SRC` table in `os/host/run_all.sh`. Tests are plain C++ programs
   with a `ck(cond, "what")` helper and print `name: N checks, M failed`.
2. **Make the test fail first.** Break the fix (delete the line, invert the
   check) and confirm the test goes red, then restore it. A test that has never
   failed has not been shown to test anything. This has caught several tests that
   passed whatever the code did.
3. **Emulator** for anything that boots, schedules, or touches littlefs:
   `./build.sh pico --emu && emu/run-tests.sh`. Write a new `emu/tests/*.robot`
   for a new behaviour (one test per file; see emu/README.md for why), and
   confirm it fails on the old image.
4. **Board** when one is attached (section 5). Where something genuinely cannot
   be verified, write **DEVICE-UNCONFIRMED** in a comment next to it.

Measuring instead of guessing:

- **Stack use:** configure a scratch build with
  `-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -fstack-usage"` and the same for CXX
  (setting `CMAKE_CXX_FLAGS` itself drops the toolchain's `-mcpu` and fails),
  then read the `.su` files and add up the frames along the call chain. On a
  board, `ps` shows each task's measured peak (`2.8K/8.0K`), and `mpu` shows the
  boot stack's.
- **Code size of one file:** `arm-none-eabi-g++ -mcpu=cortex-m33 -mthumb -O3
  -std=c++17 -fno-exceptions -fno-rtti -c file.cpp && arm-none-eabi-size`.
- **Static RAM:** `arm-none-eabi-nm -S --size-sort -C os/build_pico2_w/rpcortex_v2.elf`.

---

## 4. Rules that are not obvious from the code

### Concurrency: commands do not only run at the prompt

A command line runs on the shell's task when typed, but also from `bg` jobs,
services, the scheduler (`task add`), scripts, packages via `fw_shell_run` (Nova
D1's workers do this, on either core), and `link`'s `EXEC`. So:

- **No function-static or file-static mutable buffers** in anything a command can
  reach. Two tasks will share it. This has corrupted list files, the package
  loader's working state, `rm -r`'s path and the output capture. Use a local, a
  heap allocation for the length of the call, or a lock.
- **littlefs only under `g_fs_lock`.** `tools/check-fslock.py` (run by
  `run_all.sh`) fails the build on an `lfs_*` call outside it. Do not remove from
  a directory while walking it: find one entry, leave the walk, remove it, repeat
  (see `rm_tree` in `os/shell/fs.cpp`).
- **Two kinds of lock.** `RpcLock`/`LockGuard` is recursive, per task, and yields
  while waiting. `lock_hw_enter`/`lock_hw_exit` is a hardware spinlock with
  interrupts masked and is **not recursive**: taking it twice is a silent hang.
  Keep `lock_hw` sections a few instructions long.
- **Stacks.** `TASK_STACK_DEF` is 3 KB, `TASK_STACK_SHELL` 8 KB. Anything that
  runs a shell line gets `TASK_STACK_SHELL` (one script line that appends to a file
  measured about 3.1 KB before printf). Big buffers go on the heap, not the stack;
  the script interpreter used 5.4 KB of stack before it ran a line.

### Input and output

- **Only the shell's task reads keys.** `intr_check()` (which every package calls
  via `fw_task_should_stop`) reads the console looking for Ctrl+C and stashes
  other bytes for the line editor. A command that reads keys itself must use
  `intr_getkey()` (stash first, then the wire), never `getchar_timeout_us`, or
  keys vanish. Background tasks never read the console.
- **A transfer claims the console** (`InputClaim`), so nothing else takes its
  bytes. `put` and `link` do.
- **Two output channels.** `out_multi`/`out_write` are the data channel (what a
  pipe or `>` captures). `out_ok/out_err/out_warn/out_info` are status lines
  (`[@] [!] [?] [:]`) that stay on screen during `ls > f`. `[!]` and `[?]` lines
  also go into the 48-line log ring (usage lines do not). `out_console()` writes
  straight to the console under the output lock, for framed protocols.
- **`shell_run_line_now()`** runs a full line (`;`, `&&`, `||`, pipes, redirects)
  from code. Capture its output with `out_capture_begin_all`. It did not handle
  chaining until recently; do not reintroduce a call to `run_segment` from outside
  the prompt.

### Paths and the filesystem

- **`path_resolve()` returns false** when the result does not fit, and leaves
  `""`. Check it. `""` is the root directory to littlefs, so a caller that ignores
  the result can operate on `/`. It used to drop long names silently, turning
  `rm -r /home/root/<long name>` into `rm -r /home/root`.
- **Permission checks:** `fs_allow_read/fs_allow_change` (print a refusal) or
  `fs_may_read/fs_may_change` (silent). Every command that writes must ask, and
  so must every package file call. An admin needs sudo to change `/os` and `/etc`,
  and to read the account file, the device key and `/os/ssh`;
  the OS's own writes (the registry, the account file, `pkg install` writing
  `/os/pkg`) go to storage directly and must keep doing so, never through the
  guard.
- **Layout directories** (`/`, `/os`, `/os/pkg`, `/etc`, `/home`, `/tmp`, `/usb`)
  cannot be removed or moved, even by root. `/tmp` is emptied at every boot.
- littlefs names can be 255 characters. Do not copy a name into a small fixed
  buffer.
- The filesystem refuses writes above about 98% full; a firmware image is about
  870 KB on a 1792 KB filesystem.

### Permissions and sudo

- **Three levels** on the Command: `LEVEL_USER`, `LEVEL_ADMIN`, `LEVEL_SYSTEM` (runs
  only as `sudo <command>`). A command whose level depends on its arguments names a
  `FORMS_*` rule in `core/perms.cpp`. Those rules list the forms that are everyday
  and give everything else sudo, so a new subcommand that changes something needs
  nothing done; one that only reads goes on its rule's list. `perms_test` checks
  each command is registered with the level its rule is about: update it with
  the registration.
- **Never ask for a password where nobody can answer.** `sudo_can_ask()` decides:
  only the console's shell task and a remote login's own shell with a pty may
  prompt. A `bg` job, a service, a package's worker, `fw_shell_run` and link's
  `EXEC` (`TaskShell::noprompt`) use the session's grace or are refused with a
  line saying what to do. A prompt there waits for ever or eats keys.
- **`TaskShell::elevated`** is "runs with sudo". It is set for one command by
  `sudo`, for the life of the scheduler task, a service's task and each startup
  line (their lines come from `/etc`, which only sudo can write), inherited by
  value at `task_spawn`, and cleared by `ident_attach`. Anything new that starts
  a long-lived task from inside a sudo'd command and later runs lines a person
  did not write must clear it, as a login does.
- **The password check and the grace are `core/sudo.cpp`.** `session_reauth` is a
  wrapper over it; do not write a second cache. The grace is per session
  (interrupt domain) and per user, 5 minutes, each use restarting it.
- In a maintenance boot sudo does not ask at the console's own prompt. That is the
  recovery path for a forgotten password, and the reason nothing a remote login
  or a staged command can do may reach the console's prompt in one.

### Networking (wireless boards)

- **One owner at a time** for the radio and lwIP: `net_op_acquire()`. The CYW43
  driver only works from the core that initialised it. Read the top of
  `os/shell/net.cpp` before touching it.
- **lwIP, including its heap, exists only after the radio has been started.**
  Parsing the TLS roots (which allocate on lwIP's heap) with the radio off hung the
  board in `mem_malloc` until the watchdog fired. Check `net_radio_is_up()`.
- **Hold the network only around the lwIP calls, never across a wait.** A
  `NetOwn` (or `net_op_acquire`) held through a sleep, a reply wait or a whole
  download keeps every other user of the network waiting, an SSH session's own
  output and Ctrl+C included, and holding it defers preemption on that core.
  Take it for the open, each send or read, and the close; wait with nothing
  held (see `cmd_ping`, `net_resolve`, `lw_recv`). State an lwIP callback writes
  to must then outlive the task (a static slot, not the stack).
- **lwIP is initialised once and outlives a radio restart; the driver does
  not.** Nothing may enter lwIP while the radio is down (`TcpOwn` in nettcp.cpp),
  and `radio_down` closes every connection first (`nettcp_radio_down`,
  `http_radio_down`, `ping_radio_down`). Anything that keeps a PCB between
  `NetOwn` scopes records `net_radio_gen()` and treats a change as "gone".
  Getting this wrong hard-faulted v2.03-beta on `wifi off` with SSH on.
- **Moving to the radio's core is a scheduler operation** (`task_migrate_to`).
  `smp_test` phase 4 covers the case that broke: the core being left had nothing
  else to run.
- **Status reads must never enter the radio driver.** A battery icon that did
  froze the GUI and hung one cold boot in three.
- TLS needs about 45 KB of free heap. The clock must be set (`ntp sync`) or every
  certificate check fails with "handshake NOT DONE".

### The network bridge (no Wi-Fi)

- **The device sends no frame until a host has offered.** An image without Wi-Fi
  fetches through rpc or the browser tool (LINK.md, "The network bridge"), but
  only after link's `BRIDGE on` or a terminal's `0x1E 0x1F`, and the offer ends
  when DTR drops (`tud_cdc_line_state_cb` in shell/bridge.cpp). A frame sent to a
  plain terminal is garbage on someone's screen and a 10-second wait; anything
  that adds a frame keeps to that rule.
- **Only the console's shell task may use it** (`console_command()` in
  shell/bridge.cpp). It reads the console, which only that task may do; a `bg`
  job, a service, a package worker or a remote login gets "no network". Do not
  take an `InputClaim` in the bridge: inside `EXEC` the claim is link's, and a
  claim is per task, so releasing one there releases link's.
- **Every early stop drains.** A fetch that ends before the host's `FDONE`
  (Ctrl+C, a size cap, a full filesystem) sends `FSTOP` and reads up to `FDONE`
  (core/bridge.cpp `drain`). A host line left on the wire is read as the next
  link request, or typed at the prompt. The input door (`bridge_door`, applied by
  `intr_getkey` and the line editor through `intr_filter_key`) is the second line
  of defence: it drops any `0x1E` line arriving as keys. Ctrl+C always passes it.
- **The host decides what is fetched.** http and https only, on every redirect
  hop, an allowlist and a size cap (tools/rpc/rpctool/bridge.py; the browser's
  fetch in tools/web/js/link.js). Never add a scheme, a file read or a header the
  device can use to reach the host's own files or cookies.

### Hardware specifics

- Pin assignments for Nova D1 come from its board profiles (`os/apps/novad1/novaboard.*`), never from per-driver defaults.
- **RP2040 MPU:** the smallest region is 256 bytes. A 32-byte window is one
  enabled subregion of a 256-byte region (`mpu_v6_encode_window32`).
- **Watchdog scratch registers:** `scratch[4]` is the SDK's, `scratch[5]` the
  "deliberate reboot" flag, `scratch[6]` the boot-strike count, `scratch[7]` is
  cleared at boot so the boot ROM's mark there identifies a restart after a flash.
  Do not reuse any of them.
- The boot stack is 4 KB in SCRATCH_Y and directly above core 1's stack.

### Versions, releases, the ABI

- `os/kernel/kernel.h` holds the version the next release will carry; only
  `tools/release.py` moves it. **During a beta, a fix never bumps the version**;
  the build number is the commit count and changes by itself.
- Releases are cut only with `tools/release.py` (a dry run unless `--publish`).
  Never publish, tag or push without being asked.
- **The package ABI is append-only.** A new `fw_*` call goes at the end of the
  table with an ABI minor bump; changing an existing one breaks every installed
  package (that is a new MAJOR). See PACKAGES.md.

### Size

- pico2_w builds at -O3 (cold code at -Os). **A small change that moves the image
  a lot is usually inlining**: a helper copied into every caller. `noinline` on
  the helper fixed two of these (16 KB and 6 KB).
- pico_w has the least room. Build it before declaring a change done.

---

## 5. Working with a board

A Pico 2 W is sometimes attached over USB as `/dev/ttyACM*` (Linux),
`/dev/cu.usbmodem*` (macOS) or `COMn` (Windows), 115200 baud.

- **Use `tools/rpc`** (or LINK.md directly) to run commands and move files. It
  gives exact output, an exit status and SHA-256-checked transfers. Scraping the
  interactive console is unreliable: the line editor repaints as it goes, and
  suggestion text appears in the byte stream.
- If scraping anyway: assert DTR; send `0x03` first; append `; echo <marker>` and
  read until the marker starts a line; read whatever is waiting rather than a
  fixed count.
- **Flashing:** open the port at 1200 baud to reset into BOOTSEL, then
  `picotool load out/rpcortex-v2-<board>.uf2 && picotool reboot` (a picotool built
  with USB support), or copy the UF2 onto the BOOTSEL drive. Files on the device
  are kept. `tools/rpc flash` does this. `update from-file` is the over-the-air
  path, from a `.bin` on the device.
- **Wiping:** erasing the filesystem region (it starts at the firmware reserve
  plus the package slot; see `RPC_FW_RESERVE` in `os/CMakeLists.txt`) makes the
  next boot run first-run setup. Never wipe a board without explicit permission:
  it holds accounts and a sealed Wi-Fi password that cannot be recovered.
- After a reset the port can come back under a different name if a handle was
  still open. Look for it again.
- `/tmp` is emptied at boot; put test files elsewhere and remove them after.
- `reboot` is clean; `sreboot` is an unclean watchdog restart (for testing crash
  reports); `failboot` exercises the recovery ladder.
- Commands that change the system need `sudo`, which asks for the signed-in
  account's password (PERMISSIONS.md). `rpc` sends it with `AUTH` when the device
  asks, taking it from `$RPC_PASSWORD` or a prompt; `tools/benchmark.py` and
  `tools/devsmoke.py` take `--password-file`. Never store a password in a file in
  the repository.
- `tools/devsmoke.py` is a few-second health check; `tools/soak.py` runs for days.

---

## 6. Conventions

- **Commits:** the repository's existing author identity only. **No
  `Co-Authored-By` lines and no assistant or session trailers, ever.** One logical
  change per commit; the message says what was wrong, how it was found, what
  changed and how it was proven (numbers from the board where there are some).
- **CHANGELOG:** a line under `## Unreleased`, written for a user, for anything
  they would notice, under exactly one of New, Improved, Fixed, Security, Changed
  or Known issues (RELEASING.md says what goes where). Every push of `main` to
  GitHub is a release, cut with `tools/release.py cut --publish`.
- **Public repository:** no machine-specific paths, no notes addressed to the
  maintainer, no scratch directories. Host tools take arguments with relative
  defaults.
- **Voice:** comments and docs are plain, neutral reference prose that explains
  why. Keep the existing comment style, including the capitalised lead-ins for
  the important warnings.
- **Do not** push, publish a release, flash or wipe a board, or delete branches
  and worktrees unless the task says to.
- `CLAUDE.md` files are local, gitignored notes; do not link to them from tracked
  files.

---

## 7. Common tasks, briefly

**A new shell command:** write `static int cmd_x(int argc, char **argv)` in the
right `os/shell/*.cpp`, register it in that file's `*_register()` with a
`Command{"x", "x <args>  -- what it does", cmd_x, nullptr}` (add `LEVEL_ADMIN`
for admin-only, `LEVEL_SYSTEM` if it changes the system as PERMISSIONS.md says,
or a `FORMS_*` rule if only some forms do), and add it to `os/shell/help.cpp` and,
when it needs more than a user, the table in PERMISSIONS.md. Return 0 on success, 1 on
failure, and report failures with `out_err`. Resolve paths with `path_resolve` and
ask `fs_allow_*`. If it reads keys, use `intr_getkey`. If it waits at the prompt,
refuse to run when `task_self() != task_shell_pid()`.

**A new core module:** `os/core/x.{h,cpp}` with no hardware headers, add it to the
source list in `os/CMakeLists.txt`, write `os/host/x_test.cpp`, add
`[x_test]="$CORE/x.cpp ..."` to `os/host/run_all.sh`.

**A new package ABI call:** append it in `os/include/rpc_app.h` and `os/api.cpp`,
bump the ABI minor, validate every pointer the package passes (`ok_s`, `ok_w`),
add it to `os/host/fakehw.cpp` so packages stay testable, document it in
PACKAGES.md.

**A new link verb:** document it in LINK.md first, then implement it in
`os/shell/link.cpp` and in `tools/rpc` and `tools/web`.

**A bug report from the board:** reproduce it, find the line, write the host or
emulator test that fails, fix it, watch the test pass, then confirm on the board.
Put the measured numbers in the commit message.
