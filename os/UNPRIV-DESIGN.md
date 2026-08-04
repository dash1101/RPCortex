# Running a package unprivileged — the design

Working notes for task #75. Not shipped documentation; delete or fold into
ARCHITECTURE.md once the thing exists.

## What is already true

- `mpu_platform_init` runs per core with `PRIVDEFENA | ENABLE`. Privileged
  accesses fall back to the default map; **unprivileged accesses where no region
  matches fault.** That is the whole basis of this: describe what a package may
  touch, and everything else is denied for free.
- The loader lays an app out in two halves (`image`/`text_size` read-only,
  `data`/`data_size` writable) and a veneer pool, all 32-byte aligned and padded.
- Regions 1-3 are text/data/veneers. Region 0 is the ARMv6-M stack guard only;
  on ARMv8-M it is free because MSPLIM does that job. **Free: 0, 4, 5, 6, 7.**
- `excframe.cpp` already holds the frame arithmetic as host-testable code.

## Scope

**ARMv8-M only** (RP2350 / pico2, pico2_w). ARMv6-M regions are power-of-two
sized and self-aligned, so the five regions below would cost more RAM than the
RP2040 has. It keeps stack guards and runs packages privileged, as now.

## The five regions a package gets

| Region | Covers | Permission |
|---|---|---|
| 1 | code + rodata | read-only, executable |
| 2 | data + bss | read-write, never executable |
| 3 | veneer pool | read-only, executable |
| 4 | its own stack | read-write, never executable |
| 5 | its own heap arena | read-write, never executable |

Nothing else is reachable. Not the OS's memory, not the peripherals, not flash.

## Getting in: the package needs its own stack

`apps_launch` calls `app.entry(arg)` and `shell.cpp` calls a registered command,
both on the **shell's** 8 KB stack — which holds the shell's saved return
addresses into privileged firmware. A package that can write that stack can
direct the OS anywhere, so unprivileged mode while sharing it is not weaker
isolation, it is none.

So a shim, `app_call_unpriv(fn, arg, stack_top, ret_gate)`:

```
push  {r4-r11, lr}          on the SHELL stack
save  sp -> per-task kernel_sp
sp  = package stack top
lr  = ret_gate | 1          in the veneer pool, so it is reachable unprivileged
r0  = arg
msr CONTROL, #1 ; isb       drop to unprivileged
bx  fn
```

The return address has to be in memory the package can *execute*, and flash is
not — so the gate lives in the veneer pool, which the loader writes and which is
read-only to the package.

## Getting out of package code, and back in for an ABI call

Raising privilege needs an exception. Dropping it does not. So the two
directions are deliberately asymmetric:

**In — one `SVC`.** The veneer becomes:

```
+0  f8df c004   ldr.w ip, [pc, #4]    ; -> the word at +8
+4  df00        svc  #0
+8  .word index                        ; into the ABI table
```

The handler reads the stacked r12, bounds-checks it, and rewrites the stacked
PC to `kSymbols[index].addr` and the stacked LR to `syscall_return`, having
stashed the package's own return address per task. `CONTROL.nPRIV = 0`. The
exception return lands in the ABI function, privileged, with r0-r3 untouched.

The index goes in a register rather than in the SVC immediate because the
immediate is 8 bits and the ABI is already 156 symbols.

**Out — no exception.** The ABI function returns to `syscall_return` in flash,
still privileged, which loads the package's return address and jumps to a second
gate in the veneer pool that does the `msr CONTROL` and the final `bx`. It has
to be that way round: the instruction after the `msr` must be fetchable
unprivileged, and flash is not.

A package can of course jump to that gate itself with registers of its choosing.
It gains nothing: **`MSR` to `CONTROL` from unprivileged Thread mode is
ignored**, so it cannot raise its own privilege.

## Cost, and what it means for timing

Two exception round trips would be ~90 cycles; one plus a register dance is
roughly 40-50. Against `fw_printf` that is nothing. Against `fw_gpio_put`, which
is a handful of cycles today, it is an order of magnitude — and the DHT protocol
distinguishes a 26 µs pulse from a 70 µs one.

So a bit-banging driver genuinely cannot run behind a syscall gateway. That is
not a flaw in the gateway, it is what a privilege boundary costs.

**The number above is derived, not measured, and nothing is being built on it
until it is.** Sandbox everything first, time the round trip on a board, and let
that decide whether an exemption is needed at all and what shape it takes.

An exemption a package asks for is not a security boundary — the packages that
would need it are exactly the ones a hostile package would claim to be. If one
turns out to be necessary it has to be a grant the *user* makes, not a flag the
package sets. Designing that before knowing the number would be designing
around a guess.

The same measurement decides the soft-float question below: `calc` is nothing
but `double` arithmetic, and `Tag_ABI_HardFP_use: SP only` means every one of
those operations is an `__aeabi_*` call. If `calc` stops feeling instant, the
grouping stops being an optimization and becomes a prerequisite.

## Open, and deliberately not in this pass

- **Pointer checking at the ABI boundary.** Without it a package can say
  `fw_file_read(fd, <an OS address>, n)` and the privileged OS will do it. That
  is a *deliberate* attack, not a bug, and the stated worry is somebody else's
  package having a bug. ARMv8-M has the `TT` instruction for exactly this check;
  it needs a per-symbol table of which arguments are pointers.
- **Grouping the soft-float helpers into an unprivileged-executable flash
  region.** 41 of the 156 symbols are `__aeabi_*`, which are pure arithmetic and
  touch nothing. One more region and a linker fragment would take them off the
  syscall path entirely. Worth doing; not needed for correctness.

## Things that break and have to be fixed with it

- **`preempt_alarm` rewrites a stalled task's stacked PC to `task_forced_exit`,
  which is in flash.** An unprivileged package cannot fetch from flash, so the
  redirect would fault instead of terminating the task. The handler has to clear
  `CONTROL.nPRIV` at the same time — the task is being killed, so its privilege
  no longer matters.
- **`fw_malloc` returns OS heap**, which a sandboxed package cannot touch. It
  needs a per-package arena (region 5). Only `bench` and `stress` allocate at
  all, so a small bump allocator is enough to start.

---

## What is left, after the mechanism was built

**Measure the supervisor call.** Nothing above the mechanism should be designed
until the round trip has been timed on a board. `bench` is the vehicle: it is a
package, it is already timed, and it now runs sandboxed on RP2350 and privileged
on RP2040 — so running it on both is the measurement, and it needs no new code.
`calc` is the second half of it, because every `double` operation is an
`__aeabi_*` and every one of those is now a supervisor call.

**Two allocations per call into package code.** `app_run` and `app_run_owner`
take a 3 KB stack and a 2 KB arena from the heap for the duration and give them
back after. That is churn on a device where fragmentation is a documented
hard-stop, and `gpio list` now does it every time. Worth measuring with
`meminfo` after a few dozen package commands, and worth holding the pair per
task rather than per call if it shows.

**Pointer checking at the ABI boundary,** still. Without it a package can hand
the privileged OS an address of its choosing and have it written to. `TT` is the
instruction for it, and the table of which arguments are pointers is the work.
