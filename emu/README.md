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
boot, against Nova D1's ~61 KB image: tight, and it may not load. There is no
USB either.

What it does cover is the scheduler on both cores, littlefs on real emulated
flash, the loader, the package system, the shell and the sandbox — which is
where three of the last four bugs that reached a bench actually lived. None of
them needed a radio.

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

Boot is asserted. The shell is not, and the obstacle is the first-run wizard:
its prompts are written without a trailing newline, so `Wait For Next Line On
Uart` never returns, and `Write Line To Uart` waits for an echo that a password
field answers with bullets. `waitForEcho=false` and `Wait For Prompt On Uart`
are the right keywords and are already in the file; getting the sequence to land
reliably is the next piece of work.

A device whose registry and accounts already exist skips the wizard entirely, so
seeding the flash image with a prepared littlefs is probably the shorter path
than driving the wizard at all.
