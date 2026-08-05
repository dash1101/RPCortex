# Containing a package fault: the decision works, the resume does not

Working notes for an open bug. Delete when it is closed.

## What works

`havoc spin` — a package that never yields. The preemption interrupt (#86)
takes the CALL back and the shell carries on. Confirmed on a board.

`havoc fault` — the DECISION to contain is correct. From `logdump`:

```
26.225  fault DACCVIOL wrote where it may not cfsr=00000082 addr=10000000 sp=2003cd60
26.727  contained a fault in a package; the shell survived
```

`fault_try_contain` passed every check, `sandbox_abandon_call` rewrote the frame,
`fault_report` returned 1, and the handler performed its exception return.

## What does not

The device then dies before reaching the end of `app_run_owner`. The proof is a
negative: `'havoc' was stopped. The shell is fine.` is printed there and reaches
the log ring, and it is **not** in the report. The watchdog takes the device
some seconds later — so it HANGS, it does not fault a second time.

## The three candidates

In order along the path:

1. **`app_call_unpriv_tail`** (`os/sandbox_switch.S`). Runs on the PACKAGE's
   stack, privileged. Calls `sandbox_kernel_sp`, clears MSPLIM, switches SP to
   the firmware stack, then `pop {r3, r4-r11, pc}`.
2. **`sandbox_enter`** (`os/sandbox_rp2.cpp`), which `app_call_unpriv` returns
   into, then calls `task_rearm_protection()`.
3. **The teardown in `app_run_owner`** — `task_arena_set(nullptr)`,
   `sandbox_return`, `app_leave`.

## How the next report tells them apart

Six phase notes are in place. Whichever is LAST in the crash header's `Reached:`
line names the step that failed:

| `Reached:` | meaning |
|---|---|
| `fault: resuming into the unwind` | never left the handler, or the exception return itself failed |
| `tail: asked for the kernel stack` | the tail is running; what is left is `msr MSPLIM`, `mov sp`, `pop` |
| `sandbox: back on the firmware stack` | the tail worked; died in or after `task_rearm_protection` |
| `apps: the command returned` | died in `task_arena_set` / `sandbox_return` |
| `apps: sandbox released` | died in `app_leave` |
| `apps: left the package` | almost home; died in the report itself |

`havoc fault` is a REGISTERED COMMAND, so it unwinds through `app_run_owner` and
not `app_run_stack`. Every note above `sandbox:` used to be in the wrong
function, which is why the first attempt at this table could not distinguish
anything past `sandbox_enter`.

Also new in `logdump`, written before the resume is attempted:

```
stack <base>..<top> sp inside, N below, deepest M of S
```

`N` is how far the fault was from the BOTTOM of the package's stack — the room
the handler had to work in. The compiled tail needs about 40 bytes of it
(`sandbox_kernel_sp` is `push {r3, lr}` plus a 16-byte `bb_note_phase`), so an
`N` of any size at all rules out candidate 1 running out of room.

## Things already ruled out

- **The decision.** The log line above is written only after every check passed.
- **A duplicate package instance.** `pkg_install_file` loads what it installs and
  the boot walk loaded it again; the second copy could not register and the
  first stayed. Fixed — boot is clean and this behaviour did not change.
- **Stale fault registers.** CFSR and HFSR are sticky and nothing cleared them,
  so a later report inherited an earlier address. Fixed.
- **The report being cut off.** The contained path returned without flushing.
  Now moot: nothing is printed from the handler at all.
- **`bb_note_phase` costing a kilobyte.** It was `snprintf(dst, cap, "%s", src)`,
  a full vfprintf, in the scheduler and on package stacks. Now a bounded copy,
  16 bytes of stack, verified in the disassembly.
- **`sleep_ms` in the handler.** It goes through the alarm pool: a spinlock and
  a WFE for an interrupt that cannot fire at fault priority. Whether it returned
  at all was never established — a hang there and a successful reboot both
  arrive as "the watchdog reset the device". Replaced with `busy_wait_us_32`.
- **printf on the contained path.** The stdio mutex, the alarm pool and
  TinyUSB's device task all ran inside the fault, on the package's stack, to
  produce output nobody could read. The handler now stores eight words and the
  report is printed from task context.

## Untested suspicions, in the order worth checking

Not evidence. Written down so they are not re-derived.

- `sandbox_abandon_call` sets `depth = 0` before the return, so anything in the
  tail that asks `sandbox_in_package()` now gets a different answer than the
  code was written against.
- The handler clears MSPLIM and nothing re-arms it until
  `task_rearm_protection()`, several steps later.
- The preemption alarm goes PENDING while the handler runs and is taken the
  instant the exception returns, before the tail's first instruction.
- A lock held by firmware at the moment of the fault is never released. That
  cannot explain a death this fast, but it can explain a later wedge.

## Next, once this is located

Give the fault handler a stack of its own — a per-core buffer, switched into in
the naked handler and switched back before the exception return. That is what
would let a STACK OVERFLOW be contained as well: the reason `havoc stack` still
resets is that this handler releases MSPLIM and then runs the whole report on
the stack that just ran out. Deliberately not bundled with the diagnosis above,
because wrong assembly there costs all crash reporting at once.

## The fallback if this is not worth more time

Return to a clean reset: keep the report, which names the package and the fault,
and drop the resume. `#87` then means "a package fault is explained" rather than
"a package fault is survived". `havoc spin` (#86) is unaffected and keeps
working — that is the case that matters for a package that merely hangs.
