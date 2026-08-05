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

## Pointer checking at the ABI boundary

The last hole, and the one that made "a package cannot reach the rest of the
system" not quite true.

A sandboxed package cannot touch the OS's memory — the protection unit stops
it. But it could ASK the OS to, and the OS did as it was told. `fw_file_read(path,
buf, cap)` wrote `cap` bytes wherever `buf` pointed, and that write happened in
privileged code, where the protection unit does not apply. Every pointer
argument in the ABI was a way to read or write anything on the machine, from
inside the sandbox, without breaking out of it.

That is the difference between a sandbox that stops a package having a bug and
one that stops a package being hostile. The stated worry was always the first,
but the second is what the claim implied.

**Every pointer is now range-checked against the five regions the package was
given** — the same base and size figures the protection unit was programmed
from, so the two cannot disagree about what a package owns. 40 of the 101 ABI
entry points take a pointer; all of them check.

Not the `TT` instruction, which is what ARMv8-M provides for this. Three
reasons, in order of weight: reaching it from C needs the security extension
enabled, its result encoding is easy to decode wrongly in a way that fails
*open*, and ARMv6-M has no equivalent — so the RP2040 build would need the
arithmetic version anyway. One rule that works on both parts and can be tested
on a host is worth more than a hardware feature on one of them.

### What the checks refuse

- A buffer that starts or ends outside the package.
- A length that would wrap the address space. `p + len` overflows, and an
  overflowed comparison says yes to exactly the pointer that should be refused,
  so the arithmetic never computes `p + len`.
- A write aimed at the package's own code or veneer pool: those are readable,
  never writable.
- A string with no terminator inside the region holding it. The terminator is
  data the package controls, so it cannot be trusted to arrive — the scan stops
  at the region end rather than following it into whatever is next.
- A framebuffer whose `buf` does not hold `w * h` worth of pixels. This is the
  ABI's only nested pointer, and the size is recomputed rather than trusted.

A refusal fails the call rather than killing the package: the access does not
happen, which is the whole requirement, and a package that gets an error back
can report it. Terminating a task from inside a supervisor call is a larger and
more delicate thing to get right for no additional safety.

`mpu` reports the count. One refusal is a bug in a package; a stream of them is
a package doing it on purpose.

### What is still unchecked

**`fw_printf`'s arguments.** The format string is checked. What a `%s` inside it
points at is not: by then the arguments are on the stack in a form that cannot
be walked without parsing the format — which means a second implementation of
printf, and a second thing to get wrong. It is the one pointer the ABI still
follows without looking.

**Pointers the firmware keeps.** `rpc_register_command` is handed a name and a
help string that live in the package's code, and it stores them. They are
checked when passed, and they stay valid as long as the package is loaded — but
nothing rechecks them at use, so unloading a package while its commands are
still registered would leave them dangling. That is an existing lifetime
question rather than a new one, and it is not what this pass was about.

## Tasks a package spawns

A package escaped its own sandbox by asking for a second thread, and it did not
have to try.

`task_spawn` starts every task with `app_mem_set` false. So a task created
through `fw_task_spawn` ran with the OS's own privileges, and — because the
pointer checks read the CALLING task's regions and it had none — every pointer
it handed the ABI passed unchecked as well. Both halves of the sandbox came off
at once, for a package doing something entirely ordinary.

The task now starts in a shim that re-enters the package's sandbox before
calling anything: its own stack, its own arena from the pool, the same code and
data regions, unprivileged. The cost is one table entry and one stack frame.

Worth stating as a principle, because it is the thing that was wrong rather than
the code: **the sandbox is a property of the package, not of the call that
entered it.** Anything that starts package code has to establish it, and there
are only two such places — running a command, and spawning a task.

Two edges, both deliberate:

- A package unloaded between the spawn and the task's first turn finds no image
  and the task ends. Running the code then would be running code the heap has
  already taken back.
- With no table slot left, `fw_task_spawn` REFUSES rather than falling back to
  an ordinary task. A package that gets an unsandboxed thread because a table
  was full is worse than one that gets an error it can report.
