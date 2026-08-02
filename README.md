# RPCortex v2.0

The C++ rewrite. See `../tools/PLAN-v2.0-cpp.md` for the port plan and
`loader-spike/README.md` for the runtime application loader that the whole plan
rests on.

## Setup

Two upstream dependencies are cloned rather than committed:

```
git clone --depth 1 --branch 2.3.0 https://github.com/raspberrypi/pico-sdk.git sdk
git -C sdk submodule update --init --depth 1 lib/tinyusb
git clone --depth 1 --branch v2.11.1 https://github.com/littlefs-project/littlefs.git littlefs
```

pico-sdk **2.x** is required; 1.5.x has no RP2350 support. littlefs is pinned to
v2.11 because that is the version MicroPython's rp2 port builds, which keeps the
on-disk format compatible with a v1.0 device.

## Layout

```
loader-spike/     runtime ELF application loader — the go/no-go experiment
  include/        rpc_app.h, the only header an application includes
  firmware/       loader, symbol table, littlefs glue, fault handler, prompt
  apps/           hello / badver / faulty — the acceptance-test applications
  host/           host-side verification of the relocation engine
```
