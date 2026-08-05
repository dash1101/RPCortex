# The stress crash: what is established, and what is not

Working notes for an open bug. Delete when it is closed.

## The fault

`stress` on a Pico 2 W, always in the filesystem phase (the blackbox says
`Reached: files: single write` every time, whatever the console output appears
to show — the fault text lands wherever the USB buffer had got to).

```
cfsr=0x00000092  hfsr=0x40000000   MSTKERR stack overflow on entry
```

`cfsr 0x92` = MMFSR: DACCVIOL (0x02) + MSTKERR (0x10) + MMARVALID (0x80). An
exception frame could not be pushed because the memory under the stack pointer
was not writable.

## Established, with evidence

**It is not a stack overflow.** The name says otherwise and that cost three
rounds. Every crash reports the stack pointer near the TOP of a 7168-byte
region, with a high-water mark of 700–840 bytes:

```
sp is INSIDE it, 7048 bytes from the bottom
deepest used  : 840 of 7168 bytes
```

**Two MPU regions overlap at the stack pointer, and the overlap is real.** The
first dump of this was taken while printing, and printing yields, so another
task reprogrammed the registers mid-read — a torn read that invented an
overlap. The dump now snapshots all eight regions with interrupts masked before
printing anything, and the overlap survives:

```
region 4: 0x20049ae0..0x2004b6df (7168 B)   <- sp is here
region 5: 0x2003e9c0..0x20053fdf (87584 B)  <- sp is here
2 REGIONS OVERLAP AT SP
```

ARMv8-M calls overlapping regions UNPREDICTABLE. That is the mechanism: the
hardware may refuse an access that either region alone would permit, which is
why the faulting address is inside a region that grants RW to unprivileged code.

**Region 4 (APP_STACK) is always exactly right** — it matches the package stack
the diagnostic prints, every time.

**Region 5 (APP_ARENA) is always wrong, and differently wrong each time.**
Observed: 83,424 bytes at base 0x20043600; 87,584 bytes at base 0x2003e9c0. The
arena is `PKG_ARENA_BYTES` = 12,288. Varying values with varying bases is the
signature of uninitialised or stale memory rather than a constant miscalculation.

**The register block in these reports is meaningless.** When stacking fails
there is no frame to read, so `pc`, `lr` and `r0`–`r3` are whatever those
addresses happened to hold. One `pc` resolved to `g_current`, a variable. Only
`sp` and the fault address come from hardware registers.

## Ruled out

- **Stack sizes.** Three separate fixes (firmware reserve 2 KB, then 4 KB;
  honouring a spawned task's requested size). All were real bugs. None was this.
- **The guard armed against the wrong stack.** Real, fixed, not this.
- **`describe()` leaving stack/arena uninitialised.** Real, fixed with a memset,
  and the crash survived it — so something else writes region 5.
- **A torn read of the MPU registers.** Excluded by the atomic snapshot.
- **`task_app_mem_get` writing its output on failure.** It does not.
- **`app_leave` applying an uninitialised `saved`.** It only uses it when
  `had_saved` is true.
- **littlefs depth.** Measured with `-fstack-usage`: every frame is 100–230
  bytes, so even a deep commit chain is around 2 KB.
- **Core 1 never initialising its MPU.** `core1_main` calls
  `mpu_platform_init()`.

## Narrowed the frequency, did not remove it

Masking interrupts across the context switch — the window between
`task_ctx_switch` changing the stack pointer and `arm_protection` reprogramming
the regions — took it from roughly one crash in six runs to one in twelve. That
window was real and the fix should stay. It is not the whole story.

## Where to look next

Everything that can write `TaskAppMem::arena` / `arena_size`, and everything that
can call `set_region(MPU_RGN_APP_ARENA, ...)`:

- `sandbox_alloc` and `sandbox_describe_from` in `os/shell/apps.cpp` — both set
  arena from `mpu_v8_plan_block(PKG_ARENA_BYTES, ...)`, which cannot produce
  87 KB. Verify by printing what is stored, not what is intended.
- The pool. `sandbox_acquire` copies a `SandboxAlloc` out of `g_pool` and
  `sandbox_return` copies state back. A slot handed out while still lent, or a
  `SandboxAlloc` copied before it was filled, would put arbitrary values in the
  arena fields.
- `task_app_mem_set` / `task_app_mem_apply` — whether the struct that reaches
  the hardware is the one that was validated.
- Whether anything outside this file programs region 5.

The cheapest next step is to print `m.arena` and `m.arena_size` at the moment
`task_app_mem_set` is called, and compare against what `mpu` reports. If they
already differ at that point the caller is at fault; if they match, the
programming path is.
