# The stress crash: what is established, and what is not

Working notes for an open bug. Delete when it is closed.

## The fault

`stress` on a Pico 2 W, roughly one run in twelve. The blackbox says
`Reached: files: single write` every time, whatever the console output appears
to show — the fault text lands wherever the USB buffer had got to.

```
cfsr=0x00000092  hfsr=0x40000000   MSTKERR stack overflow on entry
```

`cfsr 0x92` = MMFSR: DACCVIOL (0x02) + MSTKERR (0x10) + MMARVALID (0x80). An
exception frame could not be pushed because the memory under the stack pointer
was not writable. `hfsr` bit 30 is FORCED: MemManage is not enabled separately,
so it escalated to HardFault.

## Established, with evidence

**It is not a stack overflow.** The name says otherwise and that cost three
rounds. Every crash reports the stack pointer near the TOP of a 7168-byte
region, with a high-water mark of 700–840 bytes:

```
sp is INSIDE it, 7048 bytes from the bottom
deepest used  : 840 of 7168 bytes
```

**The faulting address is the stack pointer itself, in all five crashes.**

| reported sp (frame ptr) | MMFAR      | difference |
|-------------------------|------------|------------|
| 0x2004b948              | 0x2004b96c | 0x24       |
| 0x2004f118              | 0x2004f13c | 0x24       |
| 0x2004daa8              | 0x2004dacc | 0x24       |
| 0x2004b8d8              | 0x2004b8fc | 0x24       |
| 0x2004b668              | 0x2004b68c | 0x24       |

Every frame pointer is 8-byte aligned. On ARMv8-M, when the interrupted stack
pointer is 4 mod 8 the hardware inserts a padding word: `frameptr = (SP - 0x20)
AND NOT(0x4)`, which is `SP - 0x24`. So MMFAR is the pre-exception stack
pointer exactly, five times out of five, and the refused access is the frame
push itself. (MLSPERR is clear, so this is not a lazy floating-point save.)

**Two MPU regions overlap at the stack pointer, and the overlap is real.** The
first dump of this was taken while printing, and printing yields, so another
task reprogrammed the registers mid-read — a torn read that invented an
overlap. The dump now snapshots all eight regions with interrupts masked before
printing anything, and the overlap survives:

```
region 4: 0x20049ae0..0x2004b6df (7168 B)   <- sp is here
region 5: 0x2003e9c0..0x20053fdf (87584 B)  <- sp is here
2 REGIONS OVERLAP AT SP — ARMv8-M calls that UNPREDICTABLE
```

ARMv8-M calls overlapping regions UNPREDICTABLE. That is the mechanism: the
hardware may refuse an access that either region alone would permit, which is
why the faulting address is inside a region that grants RW to unprivileged code.

**Region 4 (APP_STACK) is always exactly right** — it matches the package stack
the diagnostic prints, every time.

**Region 5 (APP_ARENA) is always wrong, and it is not stack litter.** Observed:
83,424 bytes at base 0x20043600; 87,584 bytes at base 0x2003e9c0. The arena is
`PKG_ARENA_BYTES` = 12,288.

Both bases and both sizes are exactly 32-byte aligned. That is not a
coincidence and it is the strongest single piece of evidence here, because
`mpu_v8_encode` rejects a base or size that is not — random values from the
stack would be refused and the region left DISABLED, 1023 times out of 1024.
An enabled region means the values passed both checks.

They pass because the mechanism forces it. If a region is left holding a NEW
base against the PREVIOUS limit:

- an inclusive limit is `base + size - 1`, so it is 31 mod 32
- a new base is 0 mod 32
- the apparent size is `limit - base + 1`, which is therefore 0 mod 32

Two real allocations, tens of kilobytes apart in the heap, described as one
region. That matches every number observed.

**Nothing in the tree can put those values in `arena_size`.** It is only ever
`mpu_v8_plan_block(PKG_ARENA_BYTES).region_bytes`, from `sandbox_alloc` or
`sandbox_describe_from`, and that function cannot return anything but 12,288
for a constant input. No code outside `os/mpu_rp2.cpp` writes an MPU register,
and no interrupt handler programs the MPU — the only one, `preempt_alarm`, just
rewrites a stacked PC.

**The register block in these reports is meaningless.** When stacking fails
there is no frame to read, so `pc`, `lr` and `r0`–`r3` are whatever those
addresses happened to hold. One `pc` resolved to `g_current`, a variable. Only
`sp` and the fault address come from hardware registers.

## The write-ordering defect (fixed)

A region is a base register and a limit register, and the enable bit lives in
the limit. `set_region` wrote the base first, which left the region ENABLED
with the new base and the previous limit until the next store landed.

That is exactly the shape the numbers describe. It is now fixed: the limit is
cleared first, so the intermediate state is a region that does not exist rather
than one describing the wrong memory. `arm_protection` and `task_app_mem_apply`
also mask interrupts across the whole sequence, so the stack limit and all five
regions land as one unit — previously only the scheduler's call was masked, and
`task_app_mem_set`, `task_app_mem_clear` and `task_rearm_protection` were not.

## Whether that was the cause is NOT yet proven

The window inside `set_region` cannot, on its own, raise this fault:

- The three unmasked callers run on the TASK's stack, privileged. No region
  covers that stack, so PRIVDEFENA gives privileged code the default memory map
  and the frame push succeeds.
- The one caller that runs on the SANDBOX stack — `arm_protection` from
  `reschedule`, where a task inside a package is on the package's stack — was
  already masked, so no exception can arrive inside it.

But every crash has the stack pointer on the SANDBOX stack. So the bad region
was live while package code was running, not during the write that created it.
For the ordering defect to be the cause, a half-finished write has to become
permanent — an interrupt that never returns to the instruction it interrupted.

So the fix is correct and necessary, and it may not be sufficient.

## What the next crash will settle

`set_region` now records, per core, what each region was ASKED for alongside
what it wrote, and sets a marker while a region is mid-write. The fault report
prints both:

- **`region N WAS MID-WRITE`** — the dump caught a half-finished write. The
  ordering defect is the cause and the fix closes it.
- **`no region was mid-write: this state was live`** — it persisted, and the
  next question is the MISMATCH line:
  - `asked for ... MISMATCH` — the hardware does not hold what `set_region`
    wrote. The programming path is at fault.
  - `asked for ... THE CALLER PASSED THIS` — `arena_size` really did hold that
    value, so `TaskAppMem` is being corrupted and the hunt moves to the struct.
  - neither line printed — region 5 matches what was last asked for and the
    reading of these dumps is wrong somewhere.

## Two failure signatures — do not confuse them

This bug is a HARD FAULT. The report has `cfsr=`, an MPU dump, and a matching
`[!] fault ...` line in `logdump`.

A run that ends with none of those, a `[POST] Last restart was the WATCHDOG`
line and a blackbox phase from much earlier than the console reached, is a
HANG, not this. Something stopped responding and the watchdog rebooted it.

That happened once, on the first build carrying the pool lock: the scheduler
calls the slot-recycled hook while holding `lock_hw`, the hook now reached the
pool, and the pool asked for the same non-recursive spinlock. `task_spawn` does
its `free()` and its recycle call outside the guard now, which is what the
comment above it always claimed. `lock_hw_enter` also records a note when a
core asks for a lock it already holds, so the next one of these names itself
instead of arriving as a silent reboot.

## Ruled out

- **Stack sizes.** Three separate fixes (firmware reserve 2 KB, then 4 KB;
  honouring a spawned task's requested size). All were real bugs. None was this.
- **The guard armed against the wrong stack.** Real, fixed, not this.
- **`describe()` leaving stack/arena uninitialised.** Real, fixed with a memset,
  and the crash survived it.
- **Stack litter in `arena`/`arena_size`.** Excluded by the alignment argument
  above.
- **A torn read of the MPU registers.** Excluded by the atomic snapshot.
- **`task_app_mem_get` writing its output on failure.** It does not.
- **`app_leave` applying an uninitialised `saved`.** It only uses it when
  `had_saved` is true.
- **littlefs depth.** Measured with `-fstack-usage`: every frame is 100–230
  bytes, so even a deep commit chain is around 2 KB.
- **Core 1 never initialising its MPU.** `core1_main` calls
  `mpu_platform_init()`.
- **A wrong `mpu_hw_t` layout.** The RP2350 SDK struct has `rbar`, `rlar`, the
  three alias pairs, a pad word and then `mair` — MAIR0 lands where it should.
- **`task_irq_save`/`task_irq_restore`.** `mrs primask; cpsid i` and a
  conditional `cpsie i`. They nest correctly.

## Narrowed the frequency, did not remove it

Masking interrupts across the context switch — the window between
`task_ctx_switch` changing the stack pointer and `arm_protection` reprogramming
the regions — took it from roughly one crash in six runs to one in twelve. That
window was real and the fix should stay. It is not the whole story.

## Also found while looking, and fixed — but not the cause

`g_pool` in `os/shell/apps.cpp` was read and written from both cores with no
lock, and `stress` runs three filesystem workers with `AFFINITY_ANY`. Two cores
could claim the same free entry, after which the winner's `sandbox_return`
cleared `lent` on a block the loser was still running on — and
`apps_pool_reclaim` was then free to hand that stack and arena back to the heap
while a package stood on them.

Now under `lock_hw_enter`, with the blocks collected inside the lock and freed
outside it. A real use-after-free, worth closing on its own account, but it
cannot change an MPU region's bounds — so it does not explain any of the
numbers above and it is not being claimed as the fix.
