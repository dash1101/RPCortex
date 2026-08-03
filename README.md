<a href="https://github.com/dash1101/RPCortex">
  <p align="center">
    <img src="RPCortex.png" alt="RPCortex">
  </p>
</a>

<p align="center">
  <a href="https://github.com/dash1101/RPCortex/releases"><img src="https://img.shields.io/github/v/release/dash1101/RPCortex?include_prereleases&label=Latest%20Release&color=2dd4be"></a>
  <a href="https://github.com/dash1101/RPCortex/issues"><img src="https://img.shields.io/github/issues/dash1101/RPCortex?color=60a5fa"></a>
  <a href="https://github.com/dash1101/RPCortex/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-RPCortex-fb923c"></a>
  <img src="https://img.shields.io/badge/built%20in-C%2B%2B17-9333ea">
  <img src="https://img.shields.io/badge/targets-RP2350%20%C2%B7%20RP2040-f472b6">
  <a href="https://rpc.novalabs.app"><img src="https://img.shields.io/badge/docs-rpc.novalabs.app-2dd4be"></a>
  <a href="https://discord.gg/hcEWwSNDBa"><img src="https://img.shields.io/badge/chat-Discord-5865F2"></a>
</p>

---

# RPCortex — Vela II — v2.0.0

RPCortex is a CLI operating system for the **Raspberry Pi Pico series**
(RP2040 / RP2350), written in **C++** and running natively on the metal. It turns
a microcontroller into something that behaves like a computer — a real
interactive shell with pipes and chaining, user accounts with hashed passwords, a
package system, WiFi, and a structured boot with hardware checks.

This is the successor to [RPCortex-OS](https://github.com/dash1101/RPCortex-OS),
which shipped through v1.0 "Vela" on MicroPython. Same shell, same commands, same
package system — without an interpreter underneath.
[Why the rewrite happened.](https://rpc.novalabs.app/switch)

It runs on hardware with 264 KB of RAM. That constraint is still the point.

> **Work in progress.** It boots, sets itself up, logs in and reaches a full
> shell, and both boards build — but it has not been through a hardware soak.
> [`STATUS.md`](STATUS.md) is honest about what works, what is unproven, and what
> is missing.

---

## What it feels like

Hold BOOTSEL, plug the board in, drag one `.uf2` onto the drive that appears.
Open a serial terminal at 115200. The logo comes up in its gradient, POST reports
what it found, and five short questions follow — root password, owner, device
name, WiFi, timezone. Then:

```
root@vela:/>
```

From there it is a shell that works. `ls` shows type, size, modified time and
name in columns. Arrows scroll the history and move the cursor mid-line; Ctrl+←
and Ctrl+→ jump by word; Tab completes commands, aliases and paths, and lists the
candidates when there is more than one. Ctrl+C stops whatever is running, at any
point.

Pipes (`cat log | grep ERROR | wc`), chaining (`wifi connect && ping 1.1.1.1`),
redirection (`sysinfo > boot.txt`) and quoting all work. `ll`, `dir`, `more`,
`del`, `free`, `gc` behave as they did on v1.

It isn't trying to be Linux. It's a $6 microcontroller. But within those
constraints, it behaves like a real system — and now there's no interpreter
between it and the hardware.

---

## What's in it

**A filesystem.** littlefs, in the same on-disk format a v1 device already uses.
`ls` `cd` `pwd` `cat` `mkdir` `rm` `mv` `cp` `rename` `touch` `tree` `df` `du`.
Copies stream rather than loading a file into RAM. `tree` draws the box
connectors. Files carry modification times, kept as a littlefs attribute since
littlefs has no timestamps of its own.

**Text processing.** `grep` `wc` `head` `tail` `find` `sort` `uniq` `hex`
`basename` `dirname` `echo`. All of them read from a pipe when given one.

**Accounts.** Salted SHA-256, admin roles, a NOPASS guest, protected root.
`whoami` `users` `mkacct` `passwd` `usermod` `rmuser` `logout`. Wrong passwords
back off on an escalating delay; three misses return to the username prompt
rather than locking the device.

**Wireless.** `wifi scan` `connect` `add` `autoconnect` `list` `forget` `auto`,
with saved networks in the registry and an SSID that can contain spaces without
quoting. Plus `ping`, `nslookup` and `ntp`.

**System.** `sysinfo` `meminfo` `uptime` `date` `ver` `pulse` `freeup` `which`
`history` `sleep` `env` `reg` `reboot` `bootloader`. `meminfo` reports the
largest allocatable block and a fragmentation percentage, not just free memory —
free bytes are not the number that predicts whether the next allocation works.

**Packages.** A package is a compiled relocatable object the OS loads at runtime,
relocates, resolves against a firmware symbol table, and runs. It registers shell
commands, which go live immediately and are swept when it unloads. `pkg install`
`pkg remove` `pkg list` `apps` `unload` `run`.

---

## Building

```
git clone --depth 1 --branch 2.3.0 https://github.com/raspberrypi/pico-sdk.git sdk
git clone --depth 1 --branch v2.11.1 https://github.com/littlefs-project/littlefs.git littlefs
./build.sh
```

`build.sh` builds both boards into `out/`, fetches the SDK submodules wireless
needs, and runs the host tests. `./build.sh pico2_w` builds one; `--clean` wipes
first.

pico-sdk **2.x** is required — 1.5.x has no RP2350 support. littlefs is pinned to
v2.11 because that is what MicroPython's rp2 port builds, which keeps the on-disk
format readable by a v1.0 device.

| Board | Chip | Notes |
|---|---|---|
| Pico 2 W | RP2350 | Primary target |
| Pico W | RP2040 | Builds and fits — the board v1.0 had to drop |

---

## Layout

```
os/               the operating system
  core/           pure logic — no hardware headers, all host-tested
  kernel/         boot, logging, heap accounting
  shell/          the command set, grouped by area
  host/           host test suite; run os/host/run_all.sh
  apps/           example packages
loader-spike/     the runtime package loader, and the experiment that proved it
tools/            host-side helpers (rpc-push.sh copies a package to a device)
```

Anything with real logic lives in `os/core/` and is compiled by the host tests
with sanitizers on. That split is deliberate: there is usually no board attached,
so a bug that can only be caught on hardware is a bug that ships.

---

## Writing a package

```cpp
#include "rpc_app.h"

RPC_APP_VER("greet", "1.0");

static int cmd_greet(int argc, char **argv) {
    fw_printf("hello from a package\n");
    return 0;
}

extern "C" int app_main(int) {
    rpc_register_command("greet", "say hello", cmd_greet);
    return 0;
}
```

Build it with `rpc_add_app(greet)` in the OS `CMakeLists.txt`, then:

```
tools/rpc-push.sh build/apps/greet.app /dev/ttyACM0
```

and `pkg install greet.app` on the device. `os/apps/greet/` is the worked
example; `os/include/rpc_app.h` is the only header a package includes.

---

## Licence

Proprietary — the RPCortex License. The source is public to read and learn from;
any other use needs written permission. See [`LICENSE`](LICENSE).

Built by [dash1101](https://github.com/dash1101) ·
[rpc.novalabs.app](https://rpc.novalabs.app) ·
[Discord](https://discord.gg/hcEWwSNDBa)
