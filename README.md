# RPCortex

A CLI operating system for microcontrollers, written in C++ and running natively
on the hardware.

This is the successor to [RPCortex-OS](https://github.com/dash1101/RPCortex-OS),
which was built on MicroPython and shipped through v1.0 "Vela". Same shell, same
commands, same accounts, same package system — without an interpreter
underneath. The reasoning behind the move is written up at
[rpc.novalabs.app/switch](https://rpc.novalabs.app/switch).

**Work in progress.** It boots, runs first-run setup, logs in and gives you a
full shell, and both targets build — but it has not been through a hardware
soak yet. `STATUS.md` tracks what works, what is unproven, and what is missing.

## Targets

| Board | Chip | Notes |
|---|---|---|
| Pico 2 W | RP2350 | Primary target |
| Pico W | RP2040 | Builds and fits — the board v1.0 had to drop |

RP2350A and RP2350B run the same binary; the difference between them is pin
count, which is board configuration rather than a build. ESP32-S3 is a separate
port and has not been started.

## Building

```
git clone --depth 1 --branch 2.3.0 https://github.com/raspberrypi/pico-sdk.git sdk
git clone --depth 1 --branch v2.11.1 https://github.com/littlefs-project/littlefs.git littlefs
./build.sh
```

`build.sh` builds both boards into `out/`, fetches the two SDK submodules that
wireless needs, and runs the host test suite. `./build.sh pico2_w` builds one
board; `--clean` wipes the build directories first.

pico-sdk **2.x** is required — 1.5.x has no RP2350 support. littlefs is pinned to
v2.11 because that is the version MicroPython's rp2 port builds, which keeps the
on-disk format readable by a v1.0 device.

## Installing

Hold BOOTSEL, plug the board in, and copy `out/rpcortex-v2-<board>.uf2` onto the
RPI-RP2 drive that appears. Then open a serial terminal at 115200 — PuTTY is what
this is developed against — and follow the first-run prompts.

There is no separate firmware-then-filesystem step any more. The whole OS is one
image.

## Layout

```
os/               the operating system
  core/           pure logic — no hardware headers, all host-tested
  kernel/         boot, logging, heap accounting
  shell/          the command set, grouped by area
  host/           host test suite; run os/host/run_all.sh
  apps/           example packages
loader-spike/     the runtime package loader, and the experiment that proved it
tools/            host-side helpers (rpc-push.sh copies a package to a device)
```

Anything with real logic lives in `os/core/` and is compiled by the host tests
with sanitizers on. That split is deliberate: there is usually no board attached,
so a bug that can only be caught on hardware is a bug that ships.

## Packages

A package is a compiled relocatable object (`.app`) that the OS loads at runtime,
relocates, resolves against a firmware symbol table, and runs. It can register
shell commands, which go live immediately and are swept when it unloads. This was
built and proven before anything else, because an OS without a package system is
a much smaller idea.

```
tools/rpc-push.sh build/apps/greet.app /dev/ttyACM0
```

then `pkg install greet.app` on the device. `os/apps/greet/` is a worked example,
and `os/include/rpc_app.h` is the only header a package includes.

## Licence

GPL-3.0. See `LICENSE`.
