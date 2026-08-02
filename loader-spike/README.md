# RPCortex v2.0 — loader spike

A proof of concept for running separately-compiled applications on a C++
RPCortex. This is the go/no-go for the whole rewrite: RPCortex is *an OS you
install packages onto*, and a C++ base that cannot load code at runtime would be
a different product.

**Verdict: viable.** The loader costs **3.3 KB of flash and no static RAM**, and
the entire spike firmware — loader, littlefs, USB serial, fault handling — uses
**4.4 KB of static RAM on an RP2040**, which is the board v1.0 had to drop.

---

## Results

### Cost

Measured with `arm-none-eabi-size` on the built objects, RP2350 unless stated.

| Component | Flash | Static RAM |
|---|---:|---:|
| **`loader.cpp` — the relocation engine** | **3,333 B** | **0 B** |
| `api.cpp` — exported symbol table (4 symbols) | 224 B | 0 B |
| `fault.cpp` — HardFault reporting | 348 B | 4 B |
| `storage.cpp` — littlefs glue | 913 B | 741 B |
| littlefs v2.11 itself | 30,464 B | 0 B |
| whole spike firmware (incl. USB, stdio, SDK) | 76,840 B | 4,028 B |
| whole spike firmware, **RP2040** | 80,908 B | 4,412 B |

The loader on Cortex-M0+ is 3,813 B — 480 B larger, because the M0+ lacks the
Thumb-2 instructions GCC uses for the bit manipulation.

### Per-app RAM

`hello.app` is a 1,756 B relocatable object containing a `.text`, a `.rodata`,
a `.bss` and an app header.

| | Bytes |
|---|---:|
| loaded image (all SHF_ALLOC sections, one allocation) | 256 |
| veneer pool allocated | 224 |
| veneer pool actually used (4 veneers × 16 B) | 64 |
| **total heap held while running** | **480** |
| after unload | 0 |

Overhead beyond the app's own sections is the veneer pool. It is currently sized
at `(relocation_count + 1) × 16`, which over-allocates about 3× — sizing it by
*distinct* call targets instead would bring it close to the 64 B actually used.
Left as-is because a spike should not optimise what it has not yet proven.

### Viability on RP2040

**Yes**, and not marginally. 4.4 KB of static RAM out of 264 KB leaves the whole
heap free, against a MicroPython v1.0 build that could not fit at all. The same
app source compiles for M0+ and produces the same relocation set.

The one caveat is unrelated to the loader: the RP2350 bootrom's A/B partition
OTA is an RP2350 feature. An RP2040 needs a custom bootloader or reflash-only
updates.

### What caused trouble

Three things, all found by testing rather than by reading:

1. **The Thumb BL addend is −4, not 0.** The ARM ELF convention folds the
   pipeline offset into the addend, so the ABI result is `(S + A) − P` with no
   separate `+4`. Treating the raw addend as part of the target lands every call
   four bytes early — it links, it loads, and it jumps into the middle of the
   previous instruction. The symptom was every veneer pointing at
   `symbol − 4`.

2. **The veneer literal has exactly one legal offset.** A Thumb LDR-literal
   computes `Align(PC, 4) + imm8×4`. For the `ldr` at +2 that is `+4 + imm8×4`,
   so `imm8 = 2` puts the word at +12 — the first 4-aligned slot past the last
   instruction. `imm8 = 1` puts it at +8, on top of the `bx ip`, and the veneer
   executes its own target word as code.

3. **A struct is the wrong way to lay out a veneer.**
   `{uint16_t[5]; uint32_t; uint16_t}` is 20 bytes after alignment padding, not
   the 16 the pool strides by. Writing 20-byte structs at 16-byte intervals
   overruns the pool and corrupts the previous veneer. It segfaulted on the host
   immediately, which is the argument for having a host test at all.

Also worth recording: `LFS_NO_MALLOC=0` **defines** the macro, and `lfs.h` gates
`lfs_file_open` on `#ifndef LFS_NO_MALLOC`. Setting it to 0 deletes the API
rather than enabling it.

### Relocation types, measured

GCC 14.2 at `-Os` emits exactly three types for real C++ on both M33 and M0+:

```
      7 R_ARM_ABS32
      6 R_ARM_THM_CALL
      1 R_ARM_THM_JUMP24     (appears with switch statements / larger functions)
```

Addresses are materialised through literal pools, so the `MOVW`/`MOVT` pair that
complicates other ARM loaders never appears — even with `-mword-relocations`.
The loader handles those two anyway, plus `REL32`, `PREL31`, `TARGET1`,
`ABS16`, `ABS8` and `THM_JUMP11`, and **fails by name** on anything else rather
than silently mis-patching.

---

## The app ABI

### Writing an app

```cpp
#include "rpc_app.h"

RPC_APP("hello");                    // name + API version, in its own section

extern "C" int app_main(int arg) {
    fw_printf("hello\n");
    return 0;
}
```

An app is a **relocatable object**, never linked:

```
arm-none-eabi-g++ -mcpu=cortex-m33 -mthumb -Os -std=c++17 \
    -fno-exceptions -fno-rtti -fno-common -fno-use-cxa-atexit \
    -ffunction-sections -fdata-sections -Iinclude \
    -c hello.cpp -o hello.app
```

`-fno-common` matters: common symbols land in `SHN_COMMON`, which the loader
does not allocate for. `-mlong-calls` is deliberately **not** used — it would
route every call through a literal pool, dodging the range problem by making all
app code slower. Handling range in the loader keeps app code as fast as
firmware code.

### What the symbol table guarantees

Apps call the firmware only through exported symbols, resolved by name at load
time. Anything not exported is an unresolved symbol and the app is **refused at
load time**, named in the error — it never becomes a null call at runtime.

Currently exported (API 1.0):

| Symbol | |
|---|---|
| `fw_printf(fmt, ...)` | formatted output |
| `fw_millis()` | milliseconds since boot |
| `fw_malloc(n)` / `fw_free(p)` | heap |

Everything is `extern "C"`. C++ name mangling is a compiler-version detail, and
an ABI that changes when the toolchain is upgraded is not an ABI.

### Versioning

Every app carries `RpcAppHeader` in `.rpc_app_header`: magic, API major, API
minor, and a name. The loader reads it **before allocating anything or trusting
any of the app's content**.

- **MAJOR** changes when a symbol is removed or changes signature. Apps built
  against a different major are refused.
- **MINOR** changes when a symbol is added. An app built against an older minor
  still runs, because everything it asked for is still there. An app built
  against a *newer* minor is refused — it may want something not present.

This is the same contract `pkg.ver` and `index.json` carry in v1.0, and it needs
the same discipline: **every exported symbol is a permanent commitment.** Keep
the surface small before publishing it.

### The veneer, and why it exists

The firmware runs from XIP flash at `0x10000000`; apps load into SRAM at
`0x20000000`. That is 256 MB apart, and a Thumb `BL` reaches ±16 MB. **Every**
call from an app into the firmware is out of range.

Each out-of-range target gets a 16-byte trampoline allocated beside the app:

```
+0   push {r0}          b401
+2   ldr  r0, [pc, #8]   4802     ; -> the literal at +12
+4   mov  ip, r0         4684
+6   pop  {r0}           bc01
+8   bx   ip             4760
+12  .word target
```

Written to be valid on Cortex-M0+ as well as M33, because the RP2040 coming back
is one of the points of the rewrite and an M33-only veneer would quietly rule it
out again. `ip` (r12) is call-clobbered under AAPCS so trashing it is legal, and
`LR` is untouched — the app's `BL` already set it, so the callee returns straight
into the app.

Veneers are deduplicated by target: an app calling `fw_printf` a dozen times gets
one trampoline.

---

## Building and running

```
export PICO_SDK_PATH=../sdk
cmake -B build -G Ninja -DPICO_BOARD=pico2_w .      # or pico_w for RP2040
ninja -C build
```

Produces `build/loader_spike.uf2` and `build/apps/*.app`.

On the device (USB serial):

```
v2> ls                       list apps in littlefs
v2> put hello.app 1756       upload an app (raw bytes follow)
v2> run hello 3              load, relocate, run, unload — with heap accounting
v2> mem                      heap and filesystem free
```

### Host verification

The relocation engine is pure computation over a byte buffer, so it runs on a
PC against the *same* app objects the device loads:

```
g++ -std=c++17 -Ifirmware -Iinclude host/host_test.cpp firmware/loader.cpp -o host_test
./host_test build/apps/hello.app build/apps/badver.app
```

It maps a fake SRAM at the device's real `0x20000000` with `MAP_FIXED_NOREPLACE`,
so the 256 MB flash-to-SRAM distance that forces veneers is reproduced exactly
and every address is one the device would really see. 29 checks: load, symbol
resolution, veneer contents byte-for-byte, the Thumb bit on the entry point,
50 load/unload cycles with no growth, version refusal, truncated and non-ELF
input.

What it **cannot** do is execute the app — the code is ARM Thumb. Running it is
the device's job.

---

## Status of the acceptance tests

| | |
|---|---|
| (a) `run hello` loads from littlefs, relocates, prints via firmware `printf` | **built, not yet run on hardware** |
| (b) runs twice with no leaked heap | **verified on host** (50 cycles, zero outstanding); device accounting is wired into `run` |
| (c) bumped API version refused with a clear message | **verified on host** |
| (d) a faulting app does not take the firmware down | **implemented, not yet run on hardware** |

Flashing this `.uf2` **erases the device filesystem**, which is why (a) and (d)
have not been run: the only board available is currently running RPCortex v1.0
with a working Nova D1 install on it. The firmware builds for both targets, and
everything that can be verified without erasing that board has been.

## What this spike deliberately does not do

No shell, no filesystem commands, no networking, no registry, no package
manager. The loader and the minimum needed to demonstrate and measure it. See
`tools/PLAN-v2.0-cpp.md` in the main workspace for what gets built on top, and
in what order.
