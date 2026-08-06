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
  permanent by accident. If it is ever temporarily off it says so at runtime,
  every time, rather than in a document nobody reads twice.

On the boards with no wireless (`pico`, `pico2`), `pkg install <name>` follows
the `#else` stub pattern `net.cpp` already uses: a clear "no wireless on this
board — use `pkg install <file>`", never a link error or a hang.

The socket layer stays behind the seam `core/` already uses. The ESP32-S3 port
does not have to happen now; it does have to not get harder.

## The TUI layer, and making it better than v1's

Not started, recorded so the shape is not re-derived later. v1 had five
independent TUIs (`settings`, `sysmon`, `fileexp`, `editor`, `desktop`), each
drawing its own boxes and reading its own keys. The improvement is not a nicer
editor, it is that the fifth app costs almost nothing to write.

- **A framework, not two apps.** Screen ownership already exists in concept from
  v1's `appkit`; here it wants a widget layer — a list, a form field, a status
  bar, a text buffer — so `edit` and `settings` are two clients of one thing.
- **Mouse input is genuinely available.** Terminals implement xterm mouse
  tracking: `\x1b[?1000h` enables reporting and `\x1b[?1006h` selects the SGR
  encoding, after which clicks, drags and the wheel arrive as ordinary escape
  sequences on the same serial line. PuTTY supports it. It costs a decoder in
  `lineedit`, not a new transport, and the parsing is pure so it host-tests.
  Worth doing: a scroll wheel in a file list is the difference between a
  demonstration and a thing people use.
- **Preemption comes first.** A full-screen app that hangs while owning the
  terminal is the worst case for a device with one serial line.

## One package, many devices

The target list is `pico`, `pico w`, `pico 2`, `pico 2 w`, Pico Plus 2 W,
ESP32-S3, ESP32-C5, ESP32 — and a package author should build once, not eight
times. That goal is right, and the obvious reading of it is impossible, so the
distinction matters.

**Three instruction sets, not one.** RP2040 is ARM Cortex-M0+ (ARMv6-M). RP2350
is Cortex-M33 (ARMv8-M) — and also ships RISC-V cores. ESP32 and ESP32-S3 are
Xtensa. ESP32-C5 and its siblings are RISC-V. No single block of native code
runs on all of those; that is a property of the silicon, not of the build system.

What *is* achievable is that a developer runs one build command, publishes one
file, and a user runs one `pkg install`. That is what "one build" has to mean
here, and it is worth designing for now while the format is young.

**Fat packages.** One `.app` containing several architecture slices, the way a
universal binary works. A small directory at the front lists each slice's
architecture and offset; the loader maps the one that matches and ignores the
rest. Consequences:

- the author builds once (a script drives the per-arch compilers) and publishes
  one artifact
- `index.json` keeps one entry per package rather than one per board
- the sizes are trivial at this scale — `greet.app` is 1732 bytes, so five
  slices is still under 10 KB
- an installer may drop the slices it will never need, so the device stores one

**Where the boundary really is.** Native code is per-architecture; everything
else is not. `core/` already compiles on a host with no target hardware at all,
which is the same property that makes it portable to Xtensa. The parts that do
not move are the context switch, storage and the network layer, and those live
in the OS rather than in packages — so a package author never meets them.

**Locking a package to one board stays legitimate.** Nova D1 targets specific
hardware and pins; a package that declares one architecture and one board is
being honest, not lazy. The index already carries `arch`, and refusing to
install where it cannot run is better than faulting later. Compatibility is a
default to make easy, not a rule to enforce.

**The ABI is the other half, and it already exists.** `RpcAppHeader` carries the
API major and minor a package was built against, and the exported services are a
stated compatibility commitment — adding one is a minor bump, changing one is a
major bump. That is what stops a package built today from breaking on next
year's firmware, and it is independent of architecture.

## Converting v1's packages

The existing repo has twenty `.pkg` archives of MicroPython. They cannot run
under v2 and are not lost: each is a small program whose behaviour is already
specified by working code, so porting is transcription rather than design.

Worth doing in rough order of use: `calc`, `i2cscan`, `gpio`, `dht`, `httpd`,
`fileexp`, `sysmon`, `speedtest`, `backup`, `ask`. Several become smaller as
`.app` files than they were as Python, because the shell already provides the
formatting and argument handling each one hand-rolled.

Nova D1 is explicitly out of scope until the OS is otherwise shipping. It is a
suite rather than a package, it targets one board, and it is the piece most
likely to want ABI additions — all of which argue for doing it last, on a stable
base, rather than discovering the requirements through it.

## Networking, and the one-owner rule

**Resolved.** The section below is kept because the reasoning is what makes the
current rule make sense, but the restriction it describes is lifted.

Two changes did it. First, only ONE task is ever inside cyw43 or lwIP: every
network operation takes `g_net_op`, an `RpcLock` that YIELDS while it waits
rather than blocking the core. Second, and more important, the questions other
tasks actually ask — is there a connection, what is the address — read a cached
struct instead of reaching into lwIP at all.

That second part is what removes the contention rather than merely managing it.
Five callers across four files ask whether the network is up: the prompt,
`sysinfo`, `compat`, `ping` and the HTTP transport. Every one of them used to
take a core-blocking lock from whatever task it happened to be on. They now read
plain memory, so they can ask from anywhere, at any time, while a download is in
flight.

A connection holds ownership from open to close, so a download and a WiFi join
can never be part-way through each other. The boot join is a background task
again, and the login prompt no longer waits on it.

## What the restriction was, and why

Worth writing down, because it cost a whole evening and the answer was
structural rather than a bug.

Moving the boot WiFi join to its own task, so the login prompt did not wait on
it, produced four different hard faults in a row — a bad data address, a null
call, corrupted registry values, a boot loop. Each looked like its own bug and
each fix revealed the next.

The cause is that **`cyw43_arch_lwip_begin` blocks the core.** It takes a mutex
by spinning, which is correct under a preemptive RTOS where the holder keeps
running on another thread. Under COOPERATIVE scheduling a task that blocks the
core on a lock held by another task on that core has stopped the only thing that
could release it. The watchdog then cleans up from wherever it happened to be,
which is why the symptom differed every time.

So: **one task, and only one, may touch cyw43 or lwIP.** Today that is whichever
task ran the command, and every network operation is therefore synchronous.

The real fix is a network task that owns every driver and lwIP call, with other
tasks posting requests to it and waiting on a result. That also gives background
downloads and a non-blocking join for free. It is not large, but it is not
something to bolt on at the end of an evening either.

Until then, two rules:

- No task other than the caller touches the network. A background join, a
  background download, a service that polls a socket — all of them need the
  network task first.
- Every cyw43 and lwIP call still needs `cyw43_arch_lwip_begin`/`end`, because
  the driver's own interrupt shares those structures. That part was missing
  entirely and is now in place.

## The TCP window has to be bigger than a TLS record

Kept because it presented as four different bugs and was one, and because the
next person to tune lwIP for RAM will reach straight for this number.

`TCP_WND` was `8 * TCP_MSS` — 11,680 bytes. A TLS record carries up to 16,384
bytes of plaintext, plus header, MAC, padding and tag: about 16.6 KB on the
wire. mbedtls cannot decrypt a PARTIAL record, and lwIP does not acknowledge
data mbedtls has not consumed. So:

- the server fills the window with part of one record
- mbedtls has nothing complete, and produces no plaintext
- nothing is consumed, so the window never advances
- the server cannot send the rest of the record

Both sides then wait until something times out. It is invisible for anything
small, because small responses arrive in small records — which is exactly how it
looked: a 1 KB manifest downloaded perfectly and a 694 KB image stopped dead at
923 bytes, one byte past the end of its 922-byte header block.

**`TCP_WND` and `MBEDTLS_SSL_IN_CONTENT_LEN` are a pair.** Reducing either
without the other reintroduces this. If RAM ever demands a smaller window, the
record size has to come down with it — which means negotiating RFC 6066
`max_fragment_length` and accepting that a server may decline.

Three earlier attempts at this were all real bugs in the receive path — refusing
data lwIP would not re-deliver, holding a buffer chain that could never drain,
acknowledging at the wrong moment — and none of them was the reason. Worth
remembering that a symptom which survives three genuine fixes is probably not
in the layer being fixed.

## The order

1. ~~Finish the current reliability pass~~ — the loader fix landed; `bench` runs
2. ~~The package fetcher~~ — done, verified by SHA-256 against the index
3. **Preemption** *(in progress; the MPU work waits on it)*
4. MPU isolation for packages
5. The six commands that need nothing but writing — `diag`, `compat`,
   `inputstat`, `regreset`, `pkgdisable`, `pkgenable`
6. The TUI layer, then `edit` and `settings` on top of it
7. `.rps` scripting, matching v1's semantics
8. Service supervision — restart a failed service instead of leaving it dead
9. Automatic crash-log-to-flash *(small, independent, do it any time)*
10. The ESP32-S3 port — `core/` moves unchanged; the context switch, storage and
    network layers do not

Preemption sits directly behind the fetcher rather than after it in spirit only:
a runaway package is a reboot today, and that stops being a rare annoyance the
moment installing someone else's package is one command. Ship the fetcher, then
preemption, before third-party package authoring is encouraged.

The rest of the feature work — the editor and `settings` (one TUI layer, two
features), `.rps`, the six commands that need nothing but writing — sits
alongside these rather than behind them. Nothing that increases the amount of
code running *concurrently* should land before preemption. That is the mistake
this document exists to avoid repeating.

---

## The concurrency faults: one bug class, two windows

A run of hard faults has followed every pass that added background work. The
signatures looked unrelated — `PRECISERR` at assorted addresses, `INVSTATE`
with `pc=0`, a PC holding ASCII (`696672fe`), and most recently
`pc=0x00000001 lr=0x00020000 sp=20029B20`. They were treated as four bugs and
patched one at a time. They are one bug, and the patches were all downstream of
it.

`pc=0x00000001` is the clearest of them. Address 0 is the bootrom on both RP2040
and RP2350, so a branch to 1 lands in ROM and executes it with garbage
registers until a load faults — which is why the fault is reported as a data
abort at an address that has nothing to do with anything. The interesting event
is not the fault. It is the branch to 1: a `pop {pc}` that came back with a
value no compiler ever wrote there.

### The invariant that is broken

**A task is advertised as schedulable before its context is safely parked.**

`pick()` is careful about the task it selects — it claims `next` by setting
`TASK_RUNNING` inside the hardware spinlock, and the comment says so: *"claims
it; no other core will pick it."* That reasoning covers `next`. Nobody applied
it to `me`.

**Window A — the switch tail.** In `reschedule()`, `me` is parked inside the
lock but its stack pointer is not written until after the lock is released:

```
if (me->info.state == TASK_RUNNING) me->info.state = park_as;   // inside the lock
...
lock_hw_exit();                                 // me is now advertised READY
if (me) task_ctx_switch(&me->sp, next->sp);     // me->sp written HERE
```

Between those two lines the other core can `pick()` `me`, find it `TASK_READY`
with `AFFINITY_ANY`, mark it `TASK_RUNNING` and context-switch into `me->sp` —
which still holds the **stale** value from its previous park. That stack region
has been overwritten many times since. Restoring from it pops garbage into
`pc`, `lr` and the callee-saved registers, which is every signature above.

This needs no sleep and no unusual timing. A plain yield with something else
runnable is enough.

**Window B — the sleep spin.** When a task sleeps and nothing else is runnable
on its core, it waits out the deadline in a spin with the lock released:

```
if (me && me->info.state == TASK_SLEEPING) {
    uint32_t wake = me->wake_at_ms;
    lock_hw_exit();                                       // SLEEPING, and live
    while ((int32_t)(task_now_ms() - wake) < 0) { ... }
    lock_hw_enter();
    me->info.state = TASK_RUNNING;
```

`runnable_on()` treats a `TASK_SLEEPING` task whose deadline has passed as
runnable. From the moment the deadline expires until the lock is retaken, the
task is both advertised as runnable and physically executing on this core. The
other core can pick it up and run the same stack.

### Why it presents as "crashes when something runs in the background"

Both windows need a second core sampling them, and an `AFFINITY_ANY` task to
steal. Both are always present:

- `core1_main` calls `task_yield()` every 200 µs — roughly 5,000 trips through
  `pick()` per second, continuously, for the whole uptime.
- The network path sleeps constantly: `task_sleep_ms(2)` in the transport retry
  loops, `(10)` on the connect wait, `(20)` and `(50)` while polling a join. A
  download or a background join opens Window B hundreds of times a second.
- `wifi-join`, package tasks and job tasks are all `AFFINITY_ANY`. The shell is
  `AFFINITY_CORE0`, so it cannot itself be stolen — which matches the symptom
  exactly: the shell is fine until something else is running.

The collision is not a narrow race. Over a few seconds of background work it is
close to certain.

### Why the test suite is green

`host/task_test.cpp` hardcodes `task_core_count()` to 1, with the note *"single-
threaded host: the cross-core guard has nothing to guard."* Every host suite is
single-core by construction, so the entire class is invisible to all of them.
Twenty-eight passing suites and this bug are not in contradiction; the suites
never looked.

### The fix

One mechanism closes both windows: a per-task `volatile bool live`, true while a
task's context is on a core, with `runnable_on()` refusing any task whose flag
is set. Fixing Window B alone leaves the common path open, and the next reflash
finds the same family of faults with a different address in it.

**Where it is cleared is the whole design.** "After the context is saved" is not
a point that exists in C: from the outgoing task's side, `task_ctx_switch` does
not return until that task is resumed, and by then the flag must be set again,
not cleared. There is no instant in its own control flow between *sp is stored*
and *the other core may pick me up*. The clear has to happen inside the switch:

```
void task_ctx_switch(void **save_sp, void *to_sp, volatile bool *live_out);
```

In `task_switch.S`, immediately after the `str` that records the stack pointer
and before the stacks are swapped — one site, in both the ARMv6-M and ARMv7-M
branches. Because the third argument arrives in `r2`, the scratch register
currently used to stage `sp` moves to `r3`:

```
    mov     r3, sp
    str     r3, [r0]        // *save_sp = sp
    dmb                     // sp must be visible before live is cleared
    movs    r3, #0
    strb    r3, [r2]        // *live_out = false
    mov     sp, r1          // switch stacks
```

**The `dmb` is not optional and the host harness cannot prove it.** The two
stores are in program order on one core, but ARM permits the other core to
observe them out of order — seeing `live == false` before the new `sp`, which
resurrects Window A exactly. A pthread harness on x86 has a far stronger memory
model than Cortex-M33 and will pass either way, so the barrier goes in from the
start rather than on the strength of a green test.

Doing it on the outgoing side also covers the `TASK_DONE` path's
`task_ctx_switch(&me->sp, back)` and the trampoline for free. The alternative —
having the incoming task clear its predecessor, as Linux does in
`finish_task_switch` — needs the clear at every resume point instead, and a
missed one leaves that task's flag set forever so `runnable_on` never picks it
again. That is a silent hang rather than a fault, and it looks exactly like the
fix having made things worse.

For completeness, the `!next` block has a fourth exit — the trailing `else` that
covers any other parked state. `reschedule` is only ever called with
`TASK_READY`, `TASK_SLEEPING` or `TASK_DONE`, all handled above, and
`runnable_on` accepts nothing else, so that path is unreachable and safe either
way.

**Deliberately not bundled:** the sleep spin should eventually stop being a spin
— a task that waits out its own deadline while holding a core is why Window B
exists at all, and a sleeper belongs parked with its deadline in the table. With
the flag in place the spin is *correct*, so that is a separate change for a
separate reflash. Two structural changes at once means a fault that persists
says nothing about which one was wrong, and that is precisely the loop this
section exists to end.

### Confirm before fixing

`bb_note_task` already records the running task, and `bb_previous()` is already
read at boot. The task name from the last crash decides this: `wifi-join`, a
package or a job task corroborates it. The shell alone would not — it is
`AFFINITY_CORE0` and cannot be stolen — and would mean something else is also
in play.

### Three findings that are separate, and not consequences of the above

**cyw43 is bound to one core, and the net task is not.** The build links
`pico_cyw43_arch_lwip_threadsafe_background`. Its async_context records
`core_num` at init and asserts on it in `process_under_lock`,
`low_priority_irq_handler` and `async_context_threadsafe_background_deinit`;
`async_context_threadsafe_background_execute_sync` has an explicit cross-core
branch guarded by `hard_assert`. Those asserts compile out in a release build,
so the wrong core does not fail loudly — it proceeds. `g_net_op` serialises
*tasks*, not cores, so the net task can begin a call on core 0 and finish it on
core 1. `wifi-join` should be `AFFINITY_CORE0`, which is one line and also takes
the net task out of the race above.

**Output is now a scheduling point.** `lock_acquire` yields when contended, so
every `out_*` call can switch tasks — a change introduced when output was
serialised, and one that has not been audited against its callers. The lwIP
receive callback is clean (it prints nothing). The fault path is not:
`task_stack_overflow` reports through `out_fatal`/`out_multi`, so a corrupt task
detected inside `reschedule` can yield back into the scheduler that just
detected it. The report of the damage must not re-enter the thing reporting it —
that path needs a non-yielding write.

**Two smaller things, worth a line each.** `crit_enter`/`crit_leave` index
`g_crit` by current core while locks are owned by tasks, and tasks migrate; a
task that takes a lock on one core and releases it on the other leaves the first
core's counter stuck. It is read only for `ps` output today, so it misreports
rather than breaks. And the lazy `if (!g_hw) g_hw = spin_lock_instance(...)` in
`lock_hw_enter` is itself racy on first concurrent use — two cores could claim
different locks. Almost certainly never fires, since core 0 is long past first
use before core 1 starts, but it is a free fix.

### The harness this needs

None of this is provable by the current suite, and everything here is meant to
be provable on the host. A two-core harness — two threads driving `reschedule()`
against a shared task table, with a spawned task that yields and sleeps in a
loop — reproduces both windows within seconds and fails before the fix. Adding
it matters more than the fix: it is what stops the next concurrency change from
shipping the same way.

### What landed

All of the above except the sleep re-architecture, which is deliberately held
back for its own reflash.

`host/smp_test.cpp` is the harness: two POSIX threads, a real mutex behind
`lock_hw_enter`, six `AFFINITY_ANY` tasks, three seconds of yielding and
sleeping. It counts how many cores believe they are running each task and fails
if that is ever more than one. Measured, with the `live` check commented out of
`runnable_on` and nothing else changed:

```
without the fix    10,787 tasks scheduled onto two cores at once
with the fix                0
```

Roughly 3,600 collisions a second, which is why this presented as "crashes a
little while after something starts in the background" rather than as a rare
race.

Two things the harness cannot prove, recorded so they are not mistaken for
tested. The `dmb` in `task_switch.S` is there on the strength of the
architecture — x86 will not reorder those two stores where Cortex-M33 will — and
the assembly itself was checked by disassembling both built images rather than
by running it:

```
str  r3, [r0]        // sp
dmb  sy
strb r3, [r2]        // live = false
mov  sp, r1          // stacks swap
```

Both branches, ARMv6-M and ARMv7-M, in that order.

Two changes came out of the same audit and are worth naming separately, because
neither is a consequence of the scheduler bug:

**Output stopped being a scheduling point.** The output lock was an `RpcLock`,
which yields when contended, so every print could switch tasks — including the
watchdog's report of a stalled task, which is emitted from inside `reschedule`
itself. It is now a non-yielding recursive lock keyed on core. Spinning is safe
here in a way it is not for the filesystem lock: nothing inside a single `out_*`
call yields, so on one core the holder always runs to completion and only the
other core can contend, for the length of one write. `out_panic_mode()` drops
the lock entirely once the system is on its way down, so a fault report cannot
hang waiting on a core that has already stopped.

**`crit_active` was disabling preemption, not just misreporting it.** The
counter was indexed by core while locks are held by tasks, and tasks migrate: one
taken on core 0 and released on core 1 decremented a counter that was never
incremented, leaving core 0 permanently "in a critical section". `preempt_decide`
reads that and returns `PREEMPT_DEFER`, so preemption on that core was off for
the rest of the boot. It is now counted per task. `cur()` gained a per-core memo
in the same change, since the critical check runs on every lock acquire and a
full-screen redraw takes the output lock hundreds of times.
