# The emulator

RPCortex boots on an emulated RP2040 under [Renode](https://renode.io), using
the community platform at [matgla/Renode_RP2040](https://github.com/matgla/Renode_RP2040).

```
emu/run.sh          boot it and print the console
emu/run-tests.sh    run the assertions in tests/
```

The first run fetches the platform, builds its peripherals (needs
`dotnet-sdk-8.0`), and builds the emulator image.

## What it can and cannot do

The model is an RP2040, so there is **no CYW43** — no WiFi, no Bluetooth — and
264 KB of RAM rather than the RP2350's 520. That leaves about 163 KB free at
boot. There is no USB either.

Nova D1 is the package that will not fit comfortably in that. The flash slot
that lets a package execute in place, and keeps only its writable half resident,
is an RP2350 feature — `RPC_PKG_SLOT_COUNT` is 0 for `pico` and `pico_w`, and an
RP2040 runs direct veneers and could not use a slot if it had one. So here it
takes the copy-to-RAM path and needs well over a hundred kilobytes in one
unbroken block: tight against 163 KB, and it may not load.

What it does cover is the scheduler on both cores, littlefs on real emulated
flash, the loader, the package system and the shell — which is where three of
the last four bugs that reached a bench actually lived. None of them needed a
radio. The sandbox is not covered either: it is ARMv8-M only, so on an RP2040
model it compiles to nothing and packages run privileged.

## Two things that are not obvious

**The console.** An `--emu` build turns UART0 on *as well as* USB rather than
swapping, because removing `pico_stdio_usb` also removes the tinyusb headers
that the mass-storage code includes directly. The SDK multiplexes stdio across
every registered driver in both directions. The emulator image therefore differs
from the bench image by one enabled output, which is what makes what happens
here worth believing about there.

**`logLevel 3 sysbus` is load-bearing.** The OS reads the RTC control register
as part of its always-on clock, the model does not implement that register, and
every read emits a warning. Thousands a second. Left on, Renode spends its time
formatting log lines and a seven-second boot does not finish inside seven
minutes. This cost most of an afternoon to notice.

## Where this stops today

**Input works.** The note that used to stand here said nothing sent to the UART
reached the OS. That was measured against a boot that had already wedged before
the shell started (the RTC stall below), so there was nothing reading. Once a
boot reaches a prompt, `Write Line To Uart` and a Monitor `WriteChar` both land:
`fs_concurrency.robot` answers the first-run wizard, logs in and types commands.
The suites are `boot`, `fs_concurrency` (two tasks on littlefs), `bg_script` (a
looping script in a background task, which used to silence the machine) and
`service_restart` (the supervisor restarting a failing service). Each is one test
in its own file: a second boot in the same Renode, after `Reset Emulation`,
stalled at the login prompt, and nothing here has needed to explain that yet.
Two things about driving it: the prompt is on the wire before a command has run,
so waiting for it says nothing about whether the command has finished (every
suite appends `; echo =done=` and waits for that); and two spaces inside an
expected line are a cell boundary to Robot, so such lines are matched as regexes.

**The RP2040 stack guard, and why boot once stopped after POST.** It used to:
core 0 spun forever on one load in `guard_ok()` (`os/core/task.cpp`) in the first
`reschedule()`. On RP2040 the stack guard is an MPU window with no access at all,
on the bottom 32 bytes of the running task's stack, and stacks start 32-aligned,
so the window always sat on the software tripwire `guard_ok()` reads at every
yield. It was also encoded as a bare 32-byte region, which is below the ARMv6-M
minimum of 256. Both are fixed: the window is one enabled subregion of a 256-byte
region, the tripwire on RP2040 is that same window, and the scheduler leaves it to
the hardware. Boot reaches the wizard in about 8 seconds with the MPU on. While a
core is stuck like that, emulated time slows to about a seventieth of normal and
the tester's timeouts (which count emulated time) never fire, so both tests carry
a wall-clock `[Timeout]`.

**No RTC.** The RP2040 model does not implement the RTC peripheral. The SDK writes
the time and then waits for a status bit the model never raises, so
`aon_timer_start_calendar` spins forever and boot stops after POST without
reaching the shell. `kernel.cpp` skips that call in emulator builds
(`RPC_CONSOLE_UART`), so the clock reports "not set" there. `boot.robot` asserts
that boot gets as far as the first-run wizard's own header. It used to stop at
"You will be asked to set a root password", which `main()` prints before the
shell task exists, so a boot that stalled after POST still passed.
