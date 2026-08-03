# Where this design stands, and where it should go

Written after a run of reliability bugs that all traced to one change. It is a
plan, not a rewrite: the scheduler works, seventeen host suites are green, and
the failure mode of "step back and rethink" at this point is churning the parts
that are already right.

## What the last five bugs actually were

Worth listing, because the pattern matters more than any one of them.

| Symptom | Cause |
|---|---|
| Board unbootable, needed a full erase | Flash writes racing core 1. XIP is unavailable during an erase, so the other core stalled mid-instruction-fetch and the erase was left half done |
| Hard lockup, no output | A finished task returned from `reschedule` and unwound into a trampoline with no return address |
| Never rebooted itself | Core 1's idle loop fed the watchdog, so a deadlocked core 0 sat behind a healthy core 1 |
| Rebooted every 8 s at the login prompt | Blocking input loops slept instead of yielding, so nothing fed the watchdog while a human typed |
| Escalation firing constantly | The stall timer lives in memory that survives a reset; after a reboot it measured against a timestamp from the previous run and wrapped to 49 days |

**Every one of them came from bringing core 1 online.** Not from the idea — from
landing it before the foundations that make a second core safe. The right
destination, reached in the wrong order, and the cost was several bricked boards.

That is the lesson to carry into what follows: the next two items below are both
foundations, and both should land *before* anything that builds on them.

## The three things worth doing, in order

### 1. Preemption

Today a task that never yields cannot be stopped. The graded watchdog *asks* it
to stop and reboots when it will not, which is honest but is not the same as
control. A `while (1)` in a package is currently a reboot.

What it takes is well-trodden and every RTOS does it identically:

- tasks run on the process stack (PSP) rather than the main stack (MSP)
- a periodic timer sets PendSV
- PendSV, at lowest priority, saves the interrupted context and switches

The payoff is not faster scheduling — cooperative scheduling is fine for this
workload. It is that **a runaway task becomes a killed task instead of a dead
device**, which is the difference between a package being a risk and a package
being a thing worth experimenting with.

It should stay cooperative *by default*: preemption used as a safety net, not as
the normal path, so the reasoning about shared state stays simple. Preempt only
a task that has exceeded its slice.

Cost: the context switch changes shape, `task_ctx_init` builds a full exception
frame, and everything touching shared state gets re-examined for interruption at
any instruction — which is why the locks came first and why they were worth it.

### 2. Fault isolation for packages

A package currently runs with the same privileges as the kernel. A bad pointer
takes down the OS, and the fault handler can name the package but not contain it.

Both chips have an MPU. Even a coarse rule — a package may write to its own
image, its own stack, and its own heap allocations, and nothing else — turns most
package bugs into a killed task with a useful message. That single change is what
makes this "a good place to build packages off of" rather than a place where one
mistake costs a reflash.

This depends on preemption: containing a fault means being able to terminate the
faulting task, which needs the machinery above.

### 3. Crash reporting that survives power loss

The ring and the black box now survive a *warm* reset, which covers the watchdog
case. They do not survive power being pulled, which is the other half.

The piece missing is small: on a boot that detects a previous crash, write the
ring and the black box to `/os/crash.log` before anything else can overwrite
them. `logdump save` already does the writing; it needs to happen automatically
rather than being remembered.

## What should not be re-litigated

These are working and the tests back them. Changing them would be motion, not
progress.

- **Cooperative by default.** The workload is a shell and a few services. The
  yield points already exist wherever `intr_check` is called, which is everywhere
  that matters.
- **One task table, both cores.** No per-core lists, no work stealing, no
  affinity except where something genuinely needs a specific core.
- **The lock design.** Yield-not-spin is required on one core, recursion is
  required by operations built from operations, and both are tested.
- **The data/status output split.** It is what makes pipes and redirection work,
  and what keeps a background task's output off someone else's redirect.
- **Pure core, hardware at the edges.** Everything in `os/core/` compiles on the
  host with sanitizers. Every scheduler bug above was found or confirmed there.

## What other systems do, and what applies here

Not everything worth copying is worth copying at this size.

**Worth taking:**

- *PendSV at lowest priority for context switching* — universal in Cortex-M
  RTOSes because it is the only correct way to switch out of an interrupt.
- *A supervisor that restarts a failed service rather than the machine* —
  systemd's actual useful idea, and it fits `service` exactly once a task can be
  killed reliably.
- *A crash report written before anything else runs* — how a kernel oops survives
  to be read.

**Not worth taking at this size:**

- *Virtual memory or full process isolation.* There is no MMU, and the MPU gives
  most of the benefit for a fraction of the complexity.
- *A general IPC layer.* Two cores and a dozen tasks do not need message queues;
  shared state behind a lock is simpler and easier to reason about.
- *Priority scheduling.* Round-robin is right until there is a task that
  genuinely must pre-empt another, and there is not one yet.

## The package fetcher

The largest remaining gap, and the one that decides whether v2 has a package
*system* or merely runs packages. `pkg install` currently takes a local file
only; there is no repo fetch, and the network layer is UDP alone — `ping`,
`nslookup` and `ntp`, with no TCP anywhere. v1 could install from the repo, so
this is not parity work with room to spare, it is the missing half.

It also unblocks more than itself: `wget`, `curl`, `runurl` and OTA `update` all
want the same HTTP client, so one piece of infrastructure closes five rows of the
"Still missing" table.

The concurrency rule above does not block it. A fetch runs in the foreground, in
one task, and adds no concurrent code — the same shape as `ping` today.

**In four stages, each testable before the next begins.** Built as one push, a
failure in the TLS layer and a failure in the socket layer look identical from
the shell, which is how a multi-pass hunt starts.

1. **Protocol parsing, no sockets** — URL splitting and an incremental response
   parser: status line, headers, `Content-Length`, chunked, redirects. Pure, so
   the boundary cases that matter (a header split mid-name, a chunk size
   arriving one digit at a time) are provoked directly instead of hoped for.
   *Landed: `core/httpparse.{h,cpp}`, 69 checks, every fixture fed at every
   possible split.*
2. **TCP transport over plain `http://`** — `http_get(url, sink, ctx)` streaming
   into a callback, proven against a local server before TLS is involved.
3. **TLS** — `altcp_tls` and mbedtls. `pico_mbedtls` is in the SDK, and flash is
   not the constraint: the image is 506 KB against a 1 MB reserve.
4. **Wire up** `pkg install <name>`, `search`, `info`, `upgrade`.

Three decisions that belong at the start rather than the middle:

- **Stream, never buffer.** A `.pkg` is 5 KB for the small ones and much larger
  for a suite like Nova D1. Bytes go to a temp file as they arrive, get verified,
  and are installed only then. This constrains the client's API shape, which is
  why it is settled at stage 2 and not discovered at stage 4.
- **TLS buffers are statically reserved.** ~32 KB of the ~426 KB free, claimed at
  boot. There is no collector here so v1's fragmentation failure cannot recur in
  the same form, but a 32 KB allocation at hour two of an uptime is still the one
  most likely to fail, and a package manager that works only on a fresh boot is
  worse than one that costs 32 KB.
- **Certificates get verified.** Shipping "verification disabled" is not a
  stage-3 shortcut to be tidied later; it is the decision most likely to become
  permanent by accident. If it is ever temporarily off it warns at runtime and
  says so in VS-V1.md.

On the boards with no wireless (`pico`, `pico2`), `pkg install <name>` follows
the `#else` stub pattern `net.cpp` already uses: a clear "no wireless on this
board — use `pkg install <file>`", never a link error or a hang.

The socket layer stays behind the seam `core/` already uses. The ESP32-S3 port
does not have to happen now; it does have to not get harder.

## The order

1. Finish the current reliability pass — confirm `stress` runs clean twice
2. Automatic crash-log-to-flash *(small, independent, do it any time)*
3. **The package fetcher, stages 1–4** *(the stated priority)*
4. Preemption *(the large one; the MPU work waits on it)*
5. MPU isolation for packages
6. Service supervision — restart a failed service instead of leaving it dead

Preemption sits directly behind the fetcher rather than after it in spirit only:
a runaway package is a reboot today, and that stops being a rare annoyance the
moment installing someone else's package is one command. Ship the fetcher, then
preemption, before third-party package authoring is encouraged.

The rest of the feature work — the editor and `settings` (one TUI layer, two
features), `.rps`, the six commands that need nothing but writing — sits
alongside these rather than behind them. Nothing that increases the amount of
code running *concurrently* should land before preemption. That is the mistake
this document exists to avoid repeating.
