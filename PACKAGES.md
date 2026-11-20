# Writing a package

A package is how software gets onto RPCortex without rebuilding the OS. It is a
small C++ program, built on a PC, copied to the device and installed with `pkg`.
Once installed it adds shell commands, and it can run tasks of its own, draw on
a terminal or a small display, and reach the board's hardware through the calls
the OS hands it.

This is the reference for building one. `os/apps/greet/` is the smallest real
example and `os/include/rpc_app.h` is the only header a package includes; both
are worth having open alongside this.

---

## What a package is

A package is a relocatable ELF object (`.app`). It is **never linked against the
firmware**. Every OS service it uses is declared `extern` in `rpc_app.h`, and when
the package is loaded the loader patches each call site from a table of exported
symbols (`os/api.cpp`). That table is the ABI.

Because nothing is linked, one `.app` runs on every board the OS supports: they
are all built for Cortex-M0+ (ARMv6-M), which the RP2350's ARMv8-M Baseline is a
superset of.

## The smallest one

```cpp
#include "rpc_app.h"

RPC_APP_VER("greet", "1.0");

static int greet_cmd(int argc, char **argv) {
    fw_printf("hello from the greet package (argc=%d)\n", argc);
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("greet", "say hello (from a loaded package)", greet_cmd);
    return 0;
}
```

- **`RPC_APP_VER(name, version)`** writes a header into the package: a magic
  number, the ABI version it was built against, its name and its own version.
  `pkg` reads the version from here, so it is the one place to bump it.
  `RPC_APP(name)` is the same with version `"1.0"`.
- **`app_main`** runs once, when the package loads. It is where commands are
  registered and anything that must exist before the first command is set up.
- **`rpc_register_command(name, help, fn)`** adds a shell command. The OS records
  which package owns it and removes it when the package unloads, so a package
  cannot leave a command behind that points into freed memory.
- A command function is an ordinary `int f(int argc, char **argv)`: `argv[0]` is
  the command name, the return value is the exit status.

## Building

1. Put the source in `os/apps/<name>/`. One `.cpp` or several.
2. Add `rpc_add_app(<name>)` to `os/CMakeLists.txt` beside the others.
3. `./build.sh pico2_w` (or any board). The result is `os/build_<board>/apps/<name>.app`;
   `make app_<name>` in the build directory rebuilds just that one.

Packages are compiled with `-mcpu=cortex-m0plus -Os -std=c++17 -fno-exceptions
-fno-rtti -fno-use-cxa-atexit`. Several source files are compiled separately and
partially linked (`ld -r`) into one object, so the files stay separate units and
the loader still sees one package.

Two checks run on every build and fail it early rather than on a device:

- **Every symbol must resolve** against `api.cpp` (`tools_checkapp.py`). A call to
  something the OS does not export, or a declaration nothing defines, is a build
  error naming the symbol.
- **No two files of one package may share a file-scope name** (`tools_checkuniq.py`).
  Separate units make that legal on the device, but the host tests compile a
  package into one unit, where it is a redefinition.

## Getting it onto a device

From a PC, over the serial console, with the command-line tool
([tools/rpc](tools/rpc/README.md)):

```
rpc install os/build_pico2_w/apps/greet.app
rpc exec greet
```

`rpc install` sends the file to `/tmp`, checks the sha256 of what landed, runs
`pkg install` on the device and removes the copy. `tools/putfile.py` does only
the sending, for a device to install from by hand (`pkg install greet.app`).

A package published to a repository is installed by name instead (`pkg install
greet` on the device, or `rpc install greet`, which downloads it on the PC so the
device needs no network); `pkg update`, `pkg upgrade`, `pkg remove` and `pkg list`
do what they say.

## The ABI

### Versions

`RPC_API_MAJOR` and `RPC_API_MINOR` in `rpc_app.h` are baked into every package.

- A **minor** bump adds calls. A package built against an older minor still runs;
  one built against a **newer** minor is refused, because it may call something
  this firmware does not have. Flash the firmware before installing a package
  built for it.
- A **major** bump removes or changes a call, and refuses every package from the
  previous major.

New calls are only ever added at the **end** of the export table. A package
installed into a flash slot has each call's table position written into it, so
anything but an append would point existing packages at the wrong function.

### What it offers

| Area | Calls | Notes |
|---|---|---|
| Output, time, memory | `fw_printf`, `fw_log`, `fw_millis`, `fw_micros`, `fw_malloc`, `fw_free` | see Memory below |
| Commands | `rpc_register_command` | removed automatically on unload |
| Tasks | `fw_task_spawn`, `fw_task_sleep_ms`, `fw_task_yield`, `fw_task_should_stop`, `fw_task_kill`, `fw_cores`, `fw_core_id` | a task may run on either core |
| Files | `fw_file_read`/`write`/`append`/`read_at`/`size`/`copy`/`rename`/`remove`/`exists`, `fw_mkdir`, `fw_dir_count`, `fw_dir_entry`, `fw_storage_roots` | paths are absolute; the SD card is read-only |
| Registry | `fw_reg_get`, `fw_reg_set`, `fw_reg_get_int`, `fw_reg_has`, `fw_reg_save` | the persistent key-value store |
| Terminal UI | `fw_tui_begin` … `fw_tui_present`, `fw_tui_poll` | a full-screen grid; only changes reach the wire |
| Framebuffer | `fw_fb_*` | drawing for a small display the package pushes itself |
| GPIO, ADC | `fw_gpio_*`, `fw_adc_*` | `fw_gpio_usable` says which pins the board leaves free |
| Buses | `fw_i2c_*`, `fw_spi_*`, `fw_uart_*` | |
| PWM, PIO | `fw_pwm_*`, `fw_pio_*` and the instruction encoders | PIO state machines are claimed and released |
| Power | `fw_power_sleep`, `fw_power_dormant`, `fw_power_source` | |
| Network | `fw_net_*`, `fw_http_get`, `fw_http_download`, `fw_tcp_*` (server side) | verified HTTPS; no outbound TCP or UDP yet |
| Shell | `fw_shell_run`, `fw_shell_run_detached`, `fw_line_read` | runs with the logged-in user's rights |
| System | `fw_time_get`/`set`, `fw_random*`, `fw_sha256`, `fw_unique_id`, `fw_board`, `fw_heap_*`, `fw_reboot` | |

The C runtime the compiler assumes is exported too: integer division, 64-bit
and soft float/double arithmetic, `memcpy`/`memset`/`memmove`/`memcmp`, the
common `str*` functions and `snprintf`. There is **no** `operator new`, no
`atoi`/`strtol` and no `sprintf`/`vsnprintf`; a package that needs to parse a
number writes the few lines it takes.

## Rules that are not obvious

**Memory.** On an RP2350 a package runs sandboxed, and `fw_malloc` hands out
memory from a 12 KB block that belongs to the current call and is given back
when it returns, so nothing allocated in one command survives to the next. A
task the package spawns is placed in the same sandbox, with a block of its own
for as long as it runs. State
that has to last lives in `static` variables, which are part of the package's
own writable half and stay for as long as it is loaded. With no `operator new`,
objects are constructed in place (`new (buffer) T()`) in static storage.

**Long loops check `fw_task_should_stop()`.** It is how Ctrl+C and `kill` reach a
package. A loop that never asks cannot be stopped. Every `fw_*` call also tells
the watchdog the task is alive, so a loop that calls nothing for seconds at a
time is indistinguishable from a hang.

**Pointers are checked.** Every pointer a package passes into the OS is checked
against the package's own memory. A bad one fails that call; it does not reach
the OS's memory.

**A package command cannot run another package's command** through
`fw_shell_run`: the shell refuses it by name. `fw_shell_run_detached` runs a line
on a task of its own and is the way to do it. Firmware commands (`wifi`, `btaudio`,
`date` …) are fine either way.

**A failed call is an answer.** File, network and bus calls return negative or
zero on failure. A package that ignores them behaves strangely rather than
crashing, which is harder to find.

## The sandbox, and how the two chips differ

On **RP2350** boards (Pico 2, Pico 2 W) a package runs unprivileged, with the
memory protection unit allowing exactly five regions: its code and read-only
data (executable, read-only), its data and bss (writable, never executable), its
call veneers, its own stack, and its own heap block. Everything else, including
the OS, the peripherals and flash, is out of reach, so hardware is only ever
touched through `fw_*` calls. A package that faults costs the command it was
running, not the device. `UNPRIV-DESIGN.md` has the reasoning.

On **RP2040** boards (Pico, Pico W) the memory protection unit is too coarse for
that layout, so packages run privileged with stack guards only. The same `.app`
runs on both; it simply has no fence on an RP2040.

## Size

By default a package is copied whole into RAM when it loads. A large package can
opt in to running from flash instead (`rpc_add_app(name PIC)`), where only its
writable half and a small table stay in RAM, but there is **one flash slot per
board** and the first package to take it keeps it.

Installing needs the package's working set in a single block of memory, and a
booted board's largest free block is about 89 KB. `os/host/realapp_test.cpp`
measures that for every package in the build and fails when one would not
install; it is worth reading its numbers when a package grows.

## Testing without a board

Packages are tested on the PC by compiling them with a fake of the calls they
use: `os/host/` has several, and `fakefw_d1.inc` is the fullest. A test drives the
package's commands or screens directly and checks what they printed, wrote or
drew. The emulator (`emu/`) runs the real firmware on an emulated RP2040 for
anything that needs the scheduler, the filesystem or the loader;
`emu/README.md` covers what it can and cannot model. `TESTING.md` covers the
host suite.

## Gaps worth knowing about before starting

- **Audio output.** There is no PCM or I2S call. A PIO state machine can drive
  an I2S DAC and PWM can drive a simple speaker, both through calls that exist;
  Bluetooth speakers are reachable through the `btaudio` command with
  `fw_shell_run`. A dedicated audio-out call would be a new ABI addition.
- **Networking** is a client for HTTP(S) and a TCP server. Outbound TCP, UDP and
  HTTP methods other than GET do not exist yet.
- **USB HID and Bluetooth GATT** are not reachable from a package.

A package that needs one of these needs an ABI addition first. Additions go at
the end of the table in `api.cpp`, with a minor bump and a line in `CHANGELOG.md`.
