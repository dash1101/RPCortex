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
some seconds later.

So the failure is between the exception return and `app_run_owner`'s tail.

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

Three phase notes are in place. Whichever is LAST in the crash header's
`Reached:` line names the step that failed:

| `Reached:` | meaning |
|---|---|
| `fault: resuming into the unwind` | died in the shim's tail — the assembly |
| `sandbox: back on the firmware stack` | the tail worked; died in or after `sandbox_enter` |
| `apps: sandbox returned` | almost home; the teardown is wrong |

## Things already ruled out

- **The decision.** The log line above is written only after every check passed.
- **A duplicate package instance.** `pkg_install_file` loads what it installs and
  the boot walk loaded it again; the second copy could not register and the
  first stayed. Fixed — boot is clean and this behaviour did not change.
- **Stale fault registers.** CFSR and HFSR are sticky and nothing cleared them,
  so a later report inherited an earlier address. Fixed.
- **The report being cut off.** The contained path returned without flushing.
  Fixed.

## Untested suspicions, in the order worth checking

Not evidence. Written down so they are not re-derived.

- `app_call_unpriv_tail` calls `sandbox_kernel_sp` **while still on the package
  stack**. A fault taken near the bottom of that stack leaves little room for
  the call, and the fault handler has already run `printf` down there.
- `sandbox_abandon_call` sets `depth = 0` before the return, so anything in the
  tail that asks `sandbox_in_package()` now gets a different answer than the
  code was written against.
- The handler clears MSPLIM and nothing re-arms it until
  `task_rearm_protection()`, several steps later.
- A lock held by firmware at the moment of the fault is never released. That
  cannot explain a death this fast, but it can explain a later wedge.

## The fallback if this is not worth more time

Return to a clean reset: keep the report, which names the package and the fault,
and drop the resume. `#87` then means "a package fault is explained" rather than
"a package fault is survived". `havoc spin` (#86) is unaffected and keeps
working — that is the case that matters for a package that merely hangs.
