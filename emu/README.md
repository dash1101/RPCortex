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

## Where this stops today: OUTPUT WORKS, INPUT DOES NOT

Worth stating plainly, because it decides what the emulator is good for.

Everything the OS PRINTS arrives. The boot is asserted on that, and a boot
regression is catchable here in seven seconds.

Nothing sent TO it arrives. Tested directly rather than inferred: with a socket
terminal connected to `sysbus.uart0`, the machine running and sitting at the
first-run prompt, sending `rpcortex\r\n` produces **zero bytes back and no
echo**. The shell echoes every character it receives, so no echo means the
character never got there.

That rules out both theories the Robot attempts suggested. It is not the wizard
prompting without a trailing newline, and it is not `Write Line To Uart` waiting
for an echo that a password field answers with bullets — both are real, both
would matter later, and neither is what is happening. Characters are not
reaching the CPU at all, which puts the problem in the model's PL011 receive
path or in what the SDK's UART stdio expects to be told about a waiting
character.

So today this is a BOOT TESTER, not a shell. Reproducing a crash that needs a
typed command — the `kill` fault, an install under memory pressure — needs the
receive path first.

Two things to try next, in order:

  * Read the platform's UART model and check whether the receive FIFO raises the
    interrupt the SDK's stdio waits on, and whether a Monitor `WriteChar` drives
    the same path a connected terminal does.
  * **Seed the flash with a prepared littlefs**, so accounts and the registry
    already exist. That skips the wizard *and* the login, and with `autonomy`
    set the device boots straight to a shell prompt. Commands can then be put in
    `/etc/startup.cfg` and their OUTPUT asserted — which reaches most of the OS
    using only the direction that already works.

The second is the better bet: it needs nothing from the model that is not
already proven.
