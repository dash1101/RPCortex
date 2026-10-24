# Testing RPCortex v2

One command runs everything that can be run without hardware:

```
cd os/host && ./run_all.sh
```

Seventy runs (every suite, plus the plain-allocator reruns), about half a minute, and the last line is the answer to one
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
| `crashlog_test` | The crash report text: header fields, log order and kind tags, a wrapped ring, and a buffer too small for it, checked for overruns with guard bytes. Rotating and writing the file needs a board. |
| `idle_test` | The idle auto-logout arithmetic: the setting parser (garbage, negatives and huge values all mean off or clamp) and the expiry test across the 32-bit millisecond wrap. The shell hook that uses it needs a logged-in session and has not run on a board. |
| `clockcfg_test` | The boot clock policy: the frequency parser (range, suffix, junk, wrap-around) and the decision made at start — apply, already there, ignore a bad value, and skip after a start that did not finish. Setting the PLL itself is confirmed on a Pico 2 W: a clean restart comes up at the stored clock, a watchdog restart skips it, the next clean one applies it again. |
| `fuzz_test` | Mutation fuzzing of the parsers that read other people's bytes: HTTP responses and URLs, the package index, the installed-package list, FAT12/16/32 volumes (with long names) and the shell's command-line scanners. 20,000 damaged inputs per target by default under AddressSan and UBSan; `RPC_FUZZ_ITERS` and `RPC_FUZZ_SEED` for a longer run. Each target also checks what its API promises, and the FAT target fails a volume that sends the reader round a loop or reads outside the volume its own header describes. It found `repo_version_cmp` reporting a suffixed version as newer than itself (so `pkg upgrade` offered the same upgrade for ever), `pkgindex_add` gluing a new package onto a line cut short by power loss, and a URL port that could overflow a 32-bit `long`. Eleven of twelve injected bugs were caught; the twelfth (the FAT chain-walk budget) only bounds how long a hostile seek takes. Not covered: the package loader and the `.app` format. |
