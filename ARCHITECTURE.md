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

## Networking cannot be split across tasks yet

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
