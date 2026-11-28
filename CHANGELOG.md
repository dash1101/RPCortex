# Changelog

Newest first. Notes for the next release are written under **Unreleased** as the work
happens: one `Summary:` line in plain words, then bullets under Added, Changed, Fixed and
Known limitations. `tools/release.py cut` turns that section into a dated entry, builds
the images, publishes the release, and updates the file the websites read. How to write
good notes is in `RELEASING.md`.

## Unreleased

Summary: 

### New

- **ask**, in the browse package (1.2): talk to Claude from the console, as v1's Ask did. `ask key`
  stores your API key in your home folder; `ask <question>` answers one question; `ask` alone is a
  conversation (`/new` starts over). It uses the Messages API with Claude Opus 5.5 by default
  (`ask model` changes it), works over Wi-Fi or through the computer, and asks for short answers so
  a reply arrives well within the device's 15-second wait. Tested end to end through the bridge
  against a stand-in with the API's shape, not yet against the real service.

- Packages can send request headers of their own (`FwHttpRequest.headers`, API 1.30): what an API
  key or a JSON API needs. Each line is checked (no line breaks inside one, and Host,
  Content-Length, Transfer-Encoding and Connection stay the firmware's), and it works the same over
  Wi-Fi and through the computer. Packages built against API 1.28 or 1.29 keep working unchanged.

### Improved

### Fixed

### Security

### Changed

- `fw_task_kill` returns -1 for a task nothing may stop (the idle task, the shell, one already
  finished), so a package can tell that from a task that is not the caller's (0). htop 1.1 says
  which.

### Known issues

## v2.0.730-beta, 2026-11-28

Summary: htop, RPC Fetch with a live mode, and packages that load only while they run, so a well-stocked board keeps its memory free

### New

- **RPC Fetch.** `fetch` (also `rpcfetch`, `neofetch`, `picofetch`) takes `-w [secs]` to redraw
  itself in place every few seconds until a key is pressed, and a colour (`fetch magenta`) as v1's
  PicoFetch did.
- **htop**, a package: the interactive process viewer laid out like the real one. A meter per
  core, memory and flash, tasks and load, a list of every task with its CPU share, stack, run time
  and where it came from, sorted by any column (F6, or P M T N, I to reverse), and F9 to stop a
  task after asking. Also answers to `top`. `pkg install htop`.
- Packages can list tasks (`fw_task_list`, API 1.29), and the header names the function keys
  (`FW_KEY_F1` to `FW_KEY_F12`).

### Improved

- Most packages now load only while their command runs (calc, gpio, dht, i2cscan, speedtest,
  websearch, backup, sysmon, htop, fileexp, ws2812, selftest), as browse already did. With all of
  them installed a Pico 2 W had 41 KB of memory free and HTTPS could not connect; now it has
  130 KB. httpd and Nova D1, which keep running in the background, stay loaded.

- `fetch` shows the power source, and the clock when the governor has lowered it.

### Fixed

- With more than about a dozen packages that load when used, the next one's commands were dropped
  without a word and the package unloaded anyway, so its command simply did not exist (`selftest`
  vanished this way). The table holds 32 commands now, and a package whose commands do not fit
  stays loaded and says so in the log.

- A package's HTTP request that got no answer at all reported a status of 0xA5A5A5A5, the stack's
  fill pattern, because the result was never initialised. It is 0 now, and selftest says "no
  answer" with the reason.

- The clock governor asked the radio chip for the power source four times a second on the W
  boards, which was all of its CPU use (0.7% of a core). It asks every 5 seconds now.

- A package setting up I2C on pins that cannot carry that bus (`i2cscan 1` used GPIO 4/5, which
  are I2C0's) hung until the stall guard stopped it. `fw_i2c_init` now refuses such pins (it
  returns -2), and every I2C read and write gives up after about 20 ms plus 1 ms a byte instead of
  waiting forever on a bus with no pull-ups.

### Security

- A package could stop any task, including the shell and the network task, whoever ran it.
  `fw_task_kill` now follows `kill`'s rule: an admin may stop anything, anyone else only what is
  theirs.

### Changed

### Known issues

## v2.0.723-beta, 2026-11-28

Summary: The device looks after its own power: idle cores sleep, a clock governor slows the CPU while quiet on battery, and `tips` recommends what is worth doing

### New

- **Recommended actions.** `tips` lists what is worth doing about this device, most important
  first, each with the command that does it: an admin account with no password, a crash on the
  last run, storage filling up, a firmware update found by `update check`, Wi-Fi never set up, the
  date not set, and power settings switched off while on battery. The login and the `settings` panel
  say how many there are. `tips dismiss <id>` hides one for that person, `tips reset` brings them
  back.
- **A clock governor.** `power governor auto` (the default) lowers the CPU clock to 48 MHz once the
  board has been quiet for 5 seconds on battery, and puts it back the moment there is work: it looks
  four times a second, and anything over half a core's worth of load is work. It never lowers the
  clock while anything timed from it is in use (an SPI, I2C or UART bus, PWM, a PIO program, the SD
  card): it reads the hardware for that, and setting any of them up puts the clock back first. `on`
  lowers it whenever quiet, plugged in too; `off` leaves the clock alone. `power status` shows the
  power source (USB or battery), what the governor is doing and why, and what is holding the clock.
  Tested on a Pico 2 W and a Pico 2 with `on`: down to the floor when quiet, back up for pretest
  (which passed at its usual speeds), and held by the SD card's bus. The battery reading and the
  current saved have not been measured: both bench boards run on USB.
- **Idle cores sleep.** A core with nothing to run used to spin at full clock: the idle task in a
  loop, core 1 with a 200 microsecond busy-wait, a sleeping task re-checking its deadline. Now it
  waits asleep until an interrupt, the other core, or at most a millisecond. The idle task sleeps
  only when nothing else is ready, and core 1 only while core 0 is idle too: sleeping beside work
  cost a package on a Pico 2 up to a fifth of its speed, and with both rules it costs nothing
  measurable (pretest's SHA-256 2084 KB/s on and off). The 128 KB transfer, command round trips,
  timers, HTTPS and ping are unchanged too. How much current it saves has not been measured.
  `power idle off` turns it off; `power status` says whether it is on.

### Improved

- An idle board no longer keeps a core busy. The USB task and the shell prompt both yielded in a
  loop forever, about a quarter of a core between them at a prompt with nothing typed. Each now
  sleeps a millisecond at a time once it has been quiet (20 ms for USB, 50 ms for the prompt), so
  typing and transfers run as before: over 20 idle seconds the USB task used 56 ms and the shell
  almost nothing.
- `fw_clock_hz` gives packages the clock their peripherals are timed from (the governor's base),
  not wherever the CPU happens to be this second.

### Fixed

### Security

### Changed

- The Pico (RP2040, no radio) standard image leaves out the add-on drivers (NFC, iButton, sub-GHz,
  LoRa) and the built-in package list, for 63 KB free in its firmware slot instead of 42. The
  drivers are for Nova D1, which runs on the Pico 2 W; `./build.sh pico --with peripherals` puts
  them back, and `pkg update` fetches the package list through the bridge the first time it is
  wanted. The other boards are unchanged.

### Known issues

- A Pico 2 W can answer pings slowly (about 150 ms) for the first one to two minutes after starting,
  with or without idle sleep and the governor; after that it is about 1 ms.
- `sd` with no card leaves the SD bus set up, and the governor stays at the base clock until the next
  start.

## v2.0.715-beta, 2026-11-27

Summary: Releases are signed, and a board checks the signature before it updates; a web server stopped and started again gets its port back at once

### New

- **Signed releases.** Every release list is signed with an RPCortex release key (Ed25519), and
  `update check` and `update install` refuse a list that is unsigned, altered, or signed by a key the
  firmware does not know, whether the board fetches it over its own Wi-Fi or through the computer.
  The list carries each image's SHA-256, so the images are covered too.
  `sudo update install --unsigned` takes an unsigned list anyway (a fork, a mirror). Installing a
  file with `update from-file`, a `.uf2` or BOOTSEL is unchanged.

### Improved

- More room on the RP2350 boards: 11 KB on the Pico 2 W and 7 KB on the Pico 2. The A/B update
  record was read with `sscanf`, which brings in about 12 KB of floating-point number parsing that
  nothing uses. A build check now refuses an image that links it.
- The Pico W and Pico carry the eleven root certificates that cover the sites the firmware and its
  packages use, and most large sites (GitHub, Google, DuckDuckGo, Wikipedia, Cloudflare, Amazon,
  Microsoft), instead of all 26: 13 KB back on the board with the least room. The RP2350 boards keep
  all 26. More can still be added in `/os/ca.pem`.

### Fixed

- `pulse set` below the default clock on a Pico 2 W made the Wi-Fi chip stop answering ("F2 not
  ready"), and the watchdog reset the board 16 seconds later. The chip's bus runs from the CPU clock
  through a fixed divider, and the bus program samples correctly only near the speed it was built
  for. The divider now follows the clock, keeping the bus at or under its usual speed, and the clock
  is changed with the bus held still. Tested on a Pico 2 W from 48 to 250 MHz with pings and a
  download at each, and with a boot clock. `pulse status` shows the bus speed. Not yet tried on a
  Pico W.
- `httpd stop` and then `httpd` on the same port was refused for two minutes ("may already be in
  use"): the connections it had served were still waiting out TCP's TIME_WAIT. A listener can take
  the port back at once now; a port another server is listening on is still refused.
- The account table had no lock while SSH and sudo checked passwords on one task and `mkacct`,
  `passwd` or `rmuser` changed it on another. It has one now, and readers get a copy of a name
  rather than a pointer into a table that a removal shifts.
- The table of detached runs is locked, since a package's task and the line it runs on can change
  it from both cores.
- Two packages loading at once on two tasks could tag each other's commands with the wrong owner.
  The owner is kept per task now.

### Security

- Release lists are signed and checked (above).

### Changed

- Checking release signatures adds about 6 KB to the RP2040 images: the Pico and Pico W have 48 KB
  left in their firmware slot, the Pico 2 W 128 KB and the Pico 2 106 KB.
- `update install` takes `--unsigned`.

### Known issues

## v2.0.709-beta, 2026-11-27

Summary: Each firmware image now says which board it is for, and the updater refuses one for another board; a selftest package checks a board in one command

### New

- `selftest` (in the package repository): checks a board in one command and prints a reading for each part: memory, a file written and read back by checksum, timers, tasks, the shell, the registry, random numbers, and the network, including that a certificate nobody should trust is refused. `selftest quick` leaves the network out.

### Improved

### Fixed

- The updater refuses firmware built for a different board. A Pico 2 and a Pico 2 W have the same chip, so the image for one went onto the other without complaint, then started, stopped answering, and stayed that way: it got far enough each time to count as started, so the board never switched back by itself. Every image now names its board, and `update online`, `update from-file` and RPC Studio's `rpc flash` (1.5.2 and later) refuse a mismatch with the reason. Images from before this release carry no name and are let through as before.

### Security

### Changed

### Known issues

## v2.0.705-beta, 2026-11-27

Summary: Security release: HTTPS certificates are now checked, SSH is harder to flood, and sudo is slower to guess. Update every board that uses Wi-Fi

### New

### Improved

### Fixed

- Two package installs at once (an SSH session and the console) shared one download file, and each could overwrite or delete the other's.
- Two packages loading at the same time could use the same working memory for relocating; loads now take turns.
- A log line written by both processor cores at once could come out as one garbled line.

### Security

- **HTTPS certificates were not checked.** The board accepted any certificate, so someone on the same network as a Wi-Fi board (a rogue access point, or a spoofed router) could have served it a firmware update or a package, with a checksum to match. Every HTTPS connection is now checked against the board's trusted roots and the host's name, and a server that fails is refused before anything is sent, with the reason ("for another name", "not a root this board trusts"). Boards that go online through RPC Studio were not affected: the computer checks those. Dates are still not checked, since the board's clock is often unset.
- The trusted roots grew from 3 to 26, enough for GitHub, Google, Wikipedia, DuckDuckGo, Cloudflare, Amazon and Microsoft sites, and they cost flash, not memory: the board reads the one a connection needs when it needs it. `/os/ca.pem` now holds only roots you add (PEM, sudo to change); `pkg certs` lists what is trusted.
- SSH limits how often connections may start: 6 per 30 seconds from one address and 12 in all, refused before any cryptography and logged once, so a flood of connections can no longer keep the board busy. A client that has not finished connecting after 15 seconds gives its place back.
- A wrong sudo password pauses longer each time it happens in a row (up to 30 seconds), which matters now that link mode's sign-in for sudo can come over SSH.

### Changed

- An HTTPS site whose certificate does not lead to one of the 26 trusted roots is refused rather than fetched. If one you need is refused, add its root to `/os/ca.pem` with sudo.

### Known issues

## v2.0.700-beta, 2026-11-26

Summary: RPCortex Studio on a phone can pair with a board and reach it over Wi-Fi: link mode runs over SSH

### New

- Link mode runs over SSH (`ssh user@board link`), so a host tool on the same network can do everything it does over the USB cable: files, packages, commands, updates. RPCortex Studio for Android uses it to reach a paired board over Wi-Fi. On SSH it ends when the connection closes, or after 90 seconds with no request (hosts send `PING` to stay), so a phone that leaves the network does not hold a session; one host tool at a time, across the console and SSH. LINK.md, "Over SSH".

### Improved

### Fixed

### Security

### Changed

- A command run directly over SSH (`ssh user@board command`) now gets what the client sends as its input, as a shell session does; before, it was thrown away.

### Known issues

## v2.0.697-beta, 2026-11-26

Summary: RP2350 boards keep two firmware partitions: updates go into the idle one, the old firmware stays to go back to, and a firmware that will not start is undone by itself

### New

- Two firmware partitions on RP2350 boards (Pico 2, Pico 2 W), on by default. An update is written into the partition that is not running, while the board keeps working, and the board restarts into it; the firmware it replaced stays in the other partition, so `update rollback --now` goes back to it, at no cost to the filesystem. If new firmware fails to start four times, the board switches back to the previous one by itself. A board is moved onto the partitions by its next update, and the `.uf2` sets them up when flashed (any board, whatever it held). The flash after the firmware does not move: files, packages and settings stay. `update rollback` says which partition is running and what is kept. RP2040 boards are unchanged.

### Improved

### Fixed

### Security

### Changed

### Known issues

## v2.0.694-beta, 2026-11-25

Summary: a network port, so rpc bridge keeps a board online for everything on it, and browse loads on a board that has been up a while

### New

- A network port: a second USB serial interface, "RPCortex Network", that carries only RPC Studio's connection. With `rpc bridge` running on the computer it is plugged into, the board is online all the time, for everything on it (background jobs, services, packages, Nova D1's menus), not only for commands typed at its console, and the console stays free. Every image has it: on a board with Wi-Fi it is used whenever Wi-Fi is not joined. Requests to the computer's own network are refused unless `rpc bridge --lan`.

- `meminfo -v` lists the largest free blocks, not just the largest: a package load needs its code and its data at once, and whether both fit is what decides it.

### Improved

### Fixed

- `browse` would not load at times ("could not be loaded ... not enough memory in one piece") on a board that had been up a while, with an SSH session open or Nova D1 loaded: it needed 43 KB in one piece and the largest was 31 KB. browse 1.1 keeps a page's text in a file and only what is on screen in memory (30 KB), and pages can be four times longer. The package loader's own working memory is now several small pieces instead of one 21 KB block, so it no longer takes the large hole a package needs.

### Security

### Changed

### Known issues

## v2.0.689-beta, 2026-11-25

Summary: browse puts the web on the console (links, forms and search, over Wi-Fi or through RPC Studio), and big packages can load only while they run

### New

- For package authors, API 1.28: `fw_http_request`, one HTTP request as a browser makes them: a method and a body, the page whatever its status, where it ended up after redirects, its content type, and a cap that keeps the start of a page too big to hold.

- `browse`, a package: the web as text on the console. Type an address or a site's name (`google` opens www.google.com) or some words to search, move between the links and boxes with the arrow keys, type into a box, press Enter; Left goes back. It shows what a page's HTML says (text, links, headings, lists, forms) and runs no JavaScript, so Google, which refuses every browser without it, has its searches sent to DuckDuckGo's text version, and the page says so. `browse -d <url>` prints a page and its links for scripts. On a board without Wi-Fi it goes online through RPC Studio. `pkg install browse`.

### Improved

- A package can load only while its command runs (`RPC_APP_F_ON_DEMAND` in its header): it is loaded once to learn its commands, then unloaded, and loaded again each time one of them is typed. `browse` does, so installing it costs nothing at rest (78 KB otherwise, from boot to power-off); `apps` lists such packages as "loads when used".

- The package loader asks for the larger half of a package first. It assumed the code was always larger; a package with a big buffer turned that round, and the code took the one hole the data needed.

### Fixed

- `fw_http_get` gave a package -1 for any reply bigger than its buffer, instead of the buffer full: the size was passed on as a limit, and the fetch refused the piece that would have filled it.

### Security

### Changed

- A board without Wi-Fi now says where its network comes from: a web request with no computer attached answers "No network. This board goes online through RPC Studio" and names the ways in (rpc term, rpc exec, the Studio app, the browser tool), first-boot setup says the same, and ping, nslookup, ntp and lanscan, which need a network of the board's own, say that web requests still work. websearch 2.3 says it too.

### Known issues

## v2.0.684-beta, 2026-11-25

Summary: lanscan finds the devices on your network, files move to and from a computer faster, and updates no longer keep a copy of the old firmware unless asked

### New

- `lanscan` lists the devices on the Wi-Fi network the board is on: each one's address and hardware address, which one is the router, and whether it answers on port 22, so a computer running SSH stands out (`-p` checks another port, `-n` none). It asks with ARP, which every device on a local network has to answer, and asks twice so phones asleep on power saving are not missed. A /24 takes about 12 seconds.

### Improved

- Sending files from a computer is about 45% faster (35 to 51 KB/s over USB on a Pico 2 W) and reading them back about twice as fast (75 to 167 KB/s). The USB console takes 512 bytes at a time instead of 64, and a `PUT` lets the computer send the next piece while the device writes the last (`READY ... window=2`, LINK.md). Studio 1.2 and later use it; older tools keep working one piece at a time.

- Writing files is about 9% faster: the filesystem programs flash a kilobyte at a time instead of 256 bytes, so it stops to park the other core a quarter as often. Copying a 380 KB file on a Pico 2 W went from 4.96 to 4.51 s, and most of what is left is the flash chip erasing (36 ms for every 4 KB), which no setting changes.

- 18 KB more memory for packages and everything else: the package loader's working buffers were held for the life of the device to be used for a second while a package loads. They are taken from the heap for the load and given back after.

### Fixed

- A crash report could name the wrong package, or none: the package being run was one setting for the whole device, so a package working on the other core, or a built-in command a package ran, changed or cleared it. Each task keeps its own now, and a task a package starts is reported as that package.

### Security

### Changed

- A copy of the old firmware is no longer kept on every update. It is as big as the firmware (848 KB of a Pico 2 W's 1792 KB filesystem), and it was taken whether or not anyone would roll back; with it off, an update also clears a copy left by earlier firmware. 'Keep rollback' in `settings` (`Update.Keep_Rollback`) turns it back on, and `sudo update rollback --save` still keeps one by hand.

### Known issues

## v2.0.676-beta, 2026-11-23

Summary: Nova D1 in the background keeps its controls when a computer connects or Ctrl+C is pressed at the console

### New

### Improved

### Fixed

- Nova D1 run in the background (`novad1 gui --bg`) lost its knob and buttons the moment anything sent the console a Ctrl+C: `rpc` connecting does, and so does stopping any command at the prompt. Its input task shared the console's Ctrl+C, and the prompt raised it for a few milliseconds even when only clearing a line. Background tasks (`bg` jobs, services, and whatever a command leaves running when it returns) no longer answer the console's Ctrl+C; `kill` still stops them, and a command in the foreground still stops on Ctrl+C with its helper tasks.

### Security

### Changed

### Known issues

## v2.0.673-beta, 2026-11-23

Summary: files over USB and the network bridge are about 50% faster, finished background jobs give their memory back, a fresh board installs from the package list it shipped with, and every image is 4.5 KB smaller

### New

### Improved

- Every image is about 4.5 KB smaller: the package list it ships with is minified (1.6 KB), and a second printf engine that came in with `mallinfo` and was never used is gone (about 3 KB). The Pico image has 64 KB of its slot free again.
- A background job gives its memory back when it finishes. It used to keep its stack and its package memory (37 KB for a job that ran a package, on a board with 319 KB) until somebody cleared it with `kill` or its slot was needed. Its line in `ps` and `jobs`, and its exit status, stay as before.
- Files sent to a board over USB arrive about 50% faster: `rpc put` went from 24 to 37 KB/s, and a download through the network bridge from 16.9 to 11.0 s for 382 KB. The console used to be read one byte per call, each a trip through the USB stack, and receiving a line took longer than writing it to flash. Link mode and the bridge read it a buffer at a time now, and give back anything past the line they wanted, so a command's Ctrl+C and keys typed during a download still arrive, in order.

### Fixed

- v2.0.640-beta to v2.0.664-beta shipped a package list naming Nova D1 1.8.0 before it was published, so on a freshly flashed board `pkg install novad1` failed its checksum. An image now carries the list as published, and `pkg install` that meets a list older than the repository (a package updated since it was fetched, or the one an older image shipped with) fetches the current list and tries once more, instead of stopping at "Checksum mismatch".
- Two tasks loading or unloading packages at the same time (a background job beside the prompt) could register one package twice, or free a table slot another was filling. The resident package table has its own lock for those changes now; looking a package up stays lock-free.
- A download bigger than the free space streamed everything that fitted (25 s for 848 KB through the bridge) before failing with a bare "could not write". A body announced as too big is refused before its first byte now, and `wget` says how big it is and how much room there is, or how far it got when the disk filled part way.

### Security

### Changed

- `websearch` 2.2 (its source is here, in os/apps): `wiki` and `search` work on a board without Wi-Fi, through the computer, and with no network say both ways to get one.

### Known issues

## v2.0.664-beta, 2026-11-23

Summary: the browser tool points at RPCortex Studio

### New

### Improved

### Fixed

- The browser tool, opened in a browser without Web Serial, still told you to `pip install -e tools/rpc`, which is gone. It links to RPCortex Studio's download instead.

### Security

### Changed

### Known issues

## v2.0.661-beta, 2026-11-23

Summary: the computer-side tools, `rpc` and `rpc tui`, move to RPCortex Studio, their own repository with a program for each system

### New

### Improved

### Fixed

### Security

### Changed

- `rpc` and `rpc tui` moved to a repository of their own, [RPCortex Studio](https://github.com/dash1101/rpcortex-studio), with their history, and get a program file per system there (Linux, macOS and Windows, with `SHA256SUMS`). `tools/rpc` is gone from this repository; the browser tool and LINK.md stay. The host suites run Studio's tests from a checkout beside this one (`RPC_STUDIO_DIR`), and say so when there is none.
- Studio 1.0.0 fixes a crash on quitting `rpc tui` under Python 3.12 and older, which the copy in this repository still had.

### Known issues

## v2.0.658-beta, 2026-11-23

Summary: a board with no network of its own can use the computer's connection, `rpc tui` is a full-screen program for a device, and the browser tool updates firmware and packages

### New

- The browser tool has a Firmware tab. It shows the device's version against the latest release, the release notes, and which boards have run it, and it updates three ways: over the air (`sudo update online` through link mode), from the computer (the page downloads the image, checks it and sends it over the cable, so a board with no Wi-Fi updates too), or by writing the `.uf2` onto the BOOTSEL drive, with "save it and drag it there" always on offer. It waits for the board to restart and says what it runs afterwards.
- The Firmware tab spots a device on v2.01-beta to v2.05-beta, which reads the new version numbers as older, says so, and moves it across with `--force`. It compares versions exactly as the device does.
- `rpc tui` is a full-screen program for a device, on Linux, macOS and Windows. It talks to the device over the link protocol, so it is a real bridge and not a wrapper that types commands. Six screens: the device and its health, files side by side with this computer, packages beside the repository, firmware against the latest release, the log and tasks, and the device's own console. It asks for the sudo password in a masked prompt when the device wants it, finds the board again after an unplug or a reboot, and lets a long copy or update be cancelled with Esc. It looks like Orbit, with its themes, and remembers the one picked. On Windows it needs `pip install windows-curses`, and says so.
- `rpc backup --with-system` keeps what a backup used to leave behind: the settings and saved Wi-Fi networks, the accounts, the device key and the SSH host key. It reads them with sudo. `rpc restore` puts them back only after naming each file and asking, then restarts the device without saving, so the restored settings stay. The saved Wi-Fi passwords open only with the device key they were sealed under, so on a different device they work only when the key from the backup goes back too.
- Every backup now ends by listing what it left out, and the option that adds each part.
- A board with no network of its own (a Pico, a Pico 2, or an image built without Wi-Fi) can use the computer's connection. With `rpc term`, `rpc exec` or the browser tool attached, `sudo pkg install <name>`, `pkg update`, `pkg upgrade`, `update check`, `sudo update install`, `wget`, `curl` and `runurl` just work: the board asks, the computer fetches the URL (checking HTTPS with its own certificates) and sends the file back over the cable. It is on by default; `rpc --no-bridge` turns it off, `--bridge-allow` limits it to some hosts and `--bridge-max` caps its size (8 MB unless told). The browser tool bridges what a web page may read, which covers the package repository and the update images.
- Link protocol 3: the `BRIDGE` request and the fetch frames that carry this, in LINK.md.

### Improved

- The browser tool's Firmware tab offers an over-the-air update on an image without Wi-Fi: the device downloads through the page, over the network bridge.
- `wget`, `curl`, `runurl`, `pkg install <name>`, `pkg update`, `pkg upgrade` and `update check` and `install` are in every image now, not only those with Wi-Fi. They cost about 18 KB on a Pico (79 KB of slot left before, about 61 KB after) and nothing on the Pico W and Pico 2 W.
- `pkg install <name>` on a board that has never fetched the package list fetches it first, instead of saying to run `pkg update`.
- `rpc term` and the browser tool's terminal keep the bridge's traffic off the screen, and hold what is typed while a download runs, sending it once it ends. Ctrl+C still goes straight through and stops the download.
- `update from-file <image> --yes` applies an image without stopping to ask, as `update online --yes` does, so a host tool can run it. The browser tool uses it to update a board from the computer in one step, and still types the command at the prompt on firmware from before it.
- The browser tool's Packages tab lists what is installed against the repository, with Update all (one password, asked first), Update and Remove for each package, and each package's architecture and ABI. `armv8m` packages are offered only to a Pico 2 or Pico 2 W.
- The browser tool shows a failure on the tab it happened on, says plainly when another program has the port open, takes Enter in the password box (it used to cancel), moves between tabs with the arrow keys, fits a phone's width, and has a light, dark or system colour switch.

### Fixed

- The last check before an update overwrites the firmware accepted an image built for either chip on either chip. It knows which layout belongs to which chip now (RP2040: the vector table at 0x100; RP2350: the table at 0 and a picobin block), so an image for the wrong chip is refused before anything is written. Tested on the host against every board's release image.
- `sreboot` restarted with a bare watchdog reset, so the next start reported a hang and saved a crash report. It is a deliberate restart now, and still the one that saves nothing first, which is what `rpc restore` needs.
- The browser tool froze when a serial port closed cleanly instead of with an error: its reader asked the finished stream for more, for ever. It treats that as the port going away now.

### Security

- The network bridge lets the computer decide what a board may fetch: only `http` and `https` (checked on every redirect), never a file on the computer, and not the computer's own `localhost` or a link-local address unless named with `--bridge-allow`. `sudo pkg install` still needs sudo, and the computer logs every request it serves.

### Changed

### Known issues

- `rpc tui` has run on Linux only. Windows needs `pip install windows-curses`; macOS and Windows are untested.
- Over the bridge, the browser can fetch from raw.githubusercontent.com (the package list, every package and the firmware images) but not GitHub release downloads, which do not allow it; the device is told so, and `rpc term` fetches them.
- `update install` on a custom image built without a feature installs the board's standard image, with every feature.

## v2.0.640-beta, 2026-11-21

Summary: version numbers become vMAJOR.MINOR.BUILD (this is v2.0.640-beta; a device on v2.01 to v2.05 moves across with `sudo update online --force`), Nova D1 1.8.0, and a Bluetooth scan no longer holds up the network.

### New

### Improved

### Fixed

- A Bluetooth scan held the network for the whole scan, so for its 6 seconds nothing else could use it and an SSH session's output stood still. It holds it only to start and stop the scan now.
### Security

### Changed

- Nova D1 is 1.8.0, the first version of the package since 1.7.1 in August: the sub-GHz library, NFC dumps, GPS waypoints and clock, the Wi-Fi scan and WiGLE wardrive, the built-in apps and sudo.
- Versions are three numbers now: `v2.0.640-beta` is generation 2, line 0, build 640. The build is the commit count, so every release has a number of its own; the middle number moves for a step in what the OS runs on (v2.1 will be the ESP32 port), and the label (`-beta`, `-rc`, none) says how finished it is. `ver` and the banner show it whole, with no separate build line. v2.00-beta to v2.05-beta keep their names. **A device on v2.01-beta to v2.05-beta sees the new numbers as older, so it moves across once with `sudo update online --force`**; after that, updates are offered as usual.
### Known issues

## v2.05-beta (build 635), 2026-11-20

Summary: images built with only the features a board needs, downloads three times faster, and more stack headroom for commands sent by host tools.

### New

- An image can be built without the parts a board does not need: `./build.sh pico_w --without wifi` (Bluetooth and SSH go with it) leaves a Pico W 529 KB of its firmware slot free instead of 75, and 93 KB more memory. `bluetooth`, `ssh`, `sd` and `peripherals` (the NFC, iButton, sub-GHz and LoRa drivers) can be left out the same way; `./build.sh --features` lists them with what each saves, and `tools/buildconf.py` asks the questions. `ver` lists the features an image was built with, a command whose feature was left out says so, and a custom image is named for what it leaves out and is never published as a release. Custom images have not yet run on a board.

### Improved

- Downloads to a file are about three times faster: 866 KB over HTTPS in 16.5 s, where it took 54.8 s, which is now as fast as the network delivers it. Every packet written used to walk the whole filesystem twice to check the file still fitted; it is measured when the file opens and every 256 KB, and estimated in between. A download that does not fit is still refused before the disk is full, and leaves no partial file.
- On the Pico and Pico 2, the network commands (`wget`, `curl`, `pkg update`, `update check` and the rest) say the board has no wireless hardware instead of trying, and the code behind them is no longer in those images, which are about 15 KB smaller.

### Fixed

- Pinging the device's own address, or 127.0.0.1, timed out. Packets to itself go through a loopback now, as on a computer, and answer in about half a millisecond.
- A command sent by a host tool ran close to the end of the console's stack: an HTTPS fetch or a package install through `rpc` peaked at 7.3 KB of 8 KB. The shell keeps its alias and redirect buffers off the stack while a command runs, and the fetch keeps its 5 KB of working state on the heap, so the same commands now peak at 4.1 KB. A redirect to a path too long to hold is an error instead of a write to the wrong place.

### Security

### Changed

### Known issues

## v2.04-beta (build 622), 2026-11-20

Summary: sudo, SSH with Ed25519 keys and per-session history, and fixes for SSH sessions losing the network, a crash on wifi off, and downloads that blocked everything else.

### New

- An SSH session keeps its own command history: up-arrow brings back that session's lines, never another person's, and `history` lists the terminal it is typed at.
- SSH accepts Ed25519 keys, which is what `ssh-keygen` makes unless told otherwise, so a computer's usual key logs in. A login with one takes about 2 seconds on a Pico 2 W, against 1.8 with ECDSA. The device's own `ssh` client also accepts a server's Ed25519 host key.
- `sudo` works the way it does on a computer. `sudo <command>` asks for the account's own password and runs that one command with sudo; the password counts for 5 minutes in that session, each use starting the time again. `sudo -v` asks now, `sudo -k` forgets it, and `sudo -l` lists what needs it. See `PERMISSIONS.md`.
- Link protocol 2 adds `AUTH`, so a host tool can give the password for sudo, and `ERR sudo` when a request needs it. `rpc` and the browser tool ask for the password once, the first time the device says so, and try again (`rpc --sudo` asks first; `$RPC_PASSWORD` is used if set). `tools/benchmark.py` and `tools/devsmoke.py` take `--password-file`, and skip the package steps and say so without one.

### Improved

- The Pico W and Pico 2 W keep their radio firmware compressed and unpack it on its way to the chip, which frees 76 KB of flash on both. A Pico W had 7 KB of its firmware slot left and now has 83 KB; a Pico 2 W goes from 105 KB to 181 KB. Every 2 KB of the firmware is checked before it is sent, so a damaged copy stops the radio from starting, with the reason in the log, instead of reaching the chip. Starting the radio borrows about 3.3 KB of memory while it unpacks and gives it back. Tested on a computer against the original firmware; not yet run on a board, so how much longer the radio takes to start is not measured yet.

### Fixed

- `ls -l`, `ls -la` and `ls -lh` took the flag for a folder name. The listing is already the long one, so those flags are accepted; an unknown one is an error.
- A command run over SSH without a terminal (`ssh dev ls > list.txt`) got the colour codes meant for a screen. They are left out when no pty was asked for.
- A login straight after another session ended could be turned away as "two sessions already". It now waits the moment the old session takes to finish: 30 logins in a row beside an open session all got in.
- `wget`, `curl`, `pkg` and `update` could not follow a redirect to a long URL: GitHub's release downloads go to a signed address of about 940 characters, which was dropped, so the 302 read as "server refused". A Location of up to 1 KB is followed now (a longer one is refused, never cut short), and the fetch keeps that state on the heap, not the stack. A download stopped with Ctrl+C says "cancelled" instead of "connection lost".
- After a restart, network commands in an SSH session failed with "Could not reach the wireless chip from this task": ping, curl and nslookup alike. A task moving to the radio's core was handed straight back to the core it was leaving whenever that core had nothing else to run, so the move never happened. The scheduler now takes it off that core.
- `wifi off` with the SSH server on restarted v2.03-beta (a hard fault in the server, then the watchdog). Every connection is now closed before the radio goes down, and the SSH server listens again after `wifi connect`.
- `ping` held the network for its whole run and paused between pings without letting anything else run on its core, and a download held the network from its first byte to its last. Over SSH that meant ping's lines arrived all at once at the end, and a Ctrl+C typed during a download could not reach the device until it finished. They now hold the network only while they use it: a download in one session and a ping in another both run, line by line, and Ctrl+C stops a download in under a second. Name lookups no longer hold it while they wait either.
- Two pings at once (the console and an SSH session) could read each other's replies. Each has its own now.
- `~` did not mean the home directory: `cd ~` and `ls ~/notes` looked for a file called `~`. It is expanded as a shell does (`~` and `~/...`, not inside quotes), Tab completes after `~/`, and a bare `cd` goes home instead of to `/`.
- `factoryreset` said that reflashing the firmware clears the accounts. It does not; the files are kept. The way back from a forgotten password is a maintenance boot (`safeboot`), where sudo at the console does not ask, and `PERMISSIONS.md` says what to do when nobody can sign in at all.
- `download <file>` asked whether the account could change the file rather than read it, so an ordinary account could not take a copy of a file it can `cat`.

### Security

- Sudo is not passed on to a script an ordinary account could have changed. A service, startup item or staged command that runs a script from `/nova/scripts`, `/tmp` or a home runs it without sudo (and logs it); only scripts in `/os` or `/etc`, or one a person ran with `sudo script`, keep it. Nova D1's self-update script moved to `/etc` for this. A password given to a host tool (`AUTH`) now stops counting when link mode ends, instead of leaving the console able to sudo for five more minutes.
- Commands that change the system need `sudo` and the admin's own password, root's included: installing, removing and upgrading packages (and `stock`), accounts (`rmuser`, `usermod`, `mkacct --admin`, another user's `passwd`), firmware (`update install`, `from-file`, `rollback --now`), what starts at boot (`startup`, `service`, `task` changes), `ssh server on` and `off`, `autonomy on` and `off`, `regreset`, `factoryreset`, `failboot`, a command staged with `safeboot`, and `reg set` of the settings that decide who gets in. Being signed in as an admin was enough before, and autonomy signs root in with no password at all. Reading, Wi-Fi, a restart and the rest of the everyday commands are as they were. The table is in `PERMISSIONS.md`.
- Changing anything in `/os` or `/etc` needs sudo for an admin too, from a command, a package or a host tool. The OS's own writes (settings, accounts, an install) are unaffected.
- Reading the account file (its password hashes can be guessed offline), the device key that seals saved Wi-Fi passwords, and the SSH host key needs sudo for an admin too. Otherwise being signed in by autonomy, or over SSH with a key, was a way to the very password sudo asks for.
- sudo never asks for a password where nobody can answer: a background job, a package, the host tools' `EXEC`, or an SSH command without a terminal (`ssh dev 'sudo ...'`, which says to use `ssh -t`). They use the session's 5 minutes, or are refused with one line saying to run `sudo -v` first. Startup items, services and scheduled tasks run with sudo, since only sudo could have put them there.
- A package run by an ordinary account could set `System.SafeBoot` and `System.SafeBootCmd`, and the next restart would run any command it liked as the console's user; it could also change where `pkg install` fetches from. Both are closed, and an admin's package needs sudo for those and the other settings that decide who gets in.
- `put`, and a package's `fw_mkdir`, did not ask whether the account may write where they were pointed. They ask now, like every other write.
- On the Pico and Pico W, where a package runs with the OS's own privileges, `run` of a package file needs sudo. Installed packages are unaffected.
- An ordinary account can change its own password with `passwd`, as the command always meant to allow; it needed an admin before. An account with no password still cannot give itself one.

### Changed

- `pkg install`, `ssh server on` and the other commands in `PERMISSIONS.md` now need `sudo` in front; without it they say so and do nothing. Scripts, startup items and host-tool commands that ran them need the word added (startup items, services and scheduled tasks already run with sudo).
- A password given to sudo, or to the commands that asked for one before (`mkacct`, `autonomy`), counts for 5 minutes instead of 2, and for that session only: the console and each SSH login have their own.
- `autonomy on` asks for the password before the warning rather than after, since `sudo` asks first.
- Nova D1's screen, run as a service, installs packages, resets settings and edits services as before. Started by hand it needs `sudo -v` at the console first. `novad1 setup` and `novad1 selfupdate` are `sudo novad1 setup` and `sudo novad1 selfupdate` now, and say so when run without it.
- The backup package keeps its snapshots in `/etc/backups`, so `backup create` and `backup restore` need `sudo` in front.
- sudo costs about 5.8 KB of flash on every board and 140 bytes of memory. With the radio firmware compressed, the Pico W has 77 KB of its slot left, the Pico 2 W 178 KB and the Pico 64 KB.

### Known issues

- sudo has been run on a Pico 2 W at the console, through `rpc` and over SSH with and without a terminal; not yet on a Pico, a Pico W or a Pico 2, and the emulator suites that now run `sudo -v` first have not been run.

## v2.03-beta (build 598), 2026-11-20

Summary: SSH on the Pico 2 W, a fix every TCP server on the device needed, and how this build measures against the first release.

### New

- `tools/benchmark.py` measures a board (boot, memory, console traffic, ping, HTTPS, upload, a package install, the CPU benchmark and a set of reliability scenarios) and compares two firmwares in percent. It drives old firmware through the prompt, so it runs on v2.00-beta too.
- SSH, on the Pico 2 W. `ssh server on` lets a computer log in with `ssh root@<device>` and get the device's shell (pipes, Tab, colours, Ctrl+C for the running command), or run one command and get its exit status. `ssh user@host` logs the device in somewhere else, with the host's key shown and remembered in `~/.ssh/known_hosts` on first use and a changed key refused. `ssh keygen` gives a user a key of their own. Key exchange curve25519-sha256 and ecdh-sha2-nistp256, an ECDSA host key, AES-GCM and AES-CTR with HMAC-SHA2-256, strict key exchange, re-keying; logins by key (ECDSA P-256, RSA with SHA-2) or password. Off until turned on, two sessions at most, no forwarding, and accounts without a password (guest as shipped) cannot log in. Wrong passwords are slowed down, six tries end a connection, and many failures in a minute shut everyone out for a minute; every login and refusal is logged. Costs 32 KB of flash on a Pico 2 W (119 KB of its slot left), and about 25 KB of memory per session, an 8 KB stack included, plus 8 KB for the server's own task while it runs. Proven on a computer against the real OpenSSH client and server, every algorithm forced in turn; not yet run on a board. See `SSH.md`.

### Improved

- `wifi power` shows the mode the chip reports beside the one that was set, so the two can be compared.
- Measured against v2.00-beta, the first release, on the same Pico 2 W with `tools/benchmark.py` (USB power, a clean boot with no packages, SSH off). Ping from a computer on the same network averages 1.1 ms where it was 94 ms (86x faster), and the worst of 40 is 2.4 ms where it was 363 ms. On battery, `wifi power auto` picks balanced, which measured about 150 ms.
- Boot to a mounted filesystem takes 2.2 s, down from 3.2 s (30% faster).
- Typing at the console sends back 2.7 bytes per key, down from 75 (96% less). It matters most on a UART console.
- An HTTPS request (`curl -I https://github.com`) takes 1.26 s, down from 2.21 s (43% faster).
- Copying a file to the board runs at 18 KB/s, up from 9.5 KB/s (89% faster). That compares link mode with typing through the old firmware's prompt, which is what a host tool gets from each.
- 304 KB is free after boot, up from 295 KB (3% more), and the largest free block is 302 KB, up from 290 KB (4% more).
- Installing Nova D1 1.7.1 from the repository, download included, takes 22.6 s, down from 23.7 s (5% faster). The CPU benchmark (`bench`) is 1% higher, within its noise.
- Seven things that went wrong on v2.00-beta are now checked by the benchmark, and all of them work: `;` inside a script, `wc -l`, a looping script under `bg` (it restarted the board), a package's output through a pipe, a file name of 40 characters or more, `rm` of a system folder (it removed `/usb`), and `compat` with the radio off (it restarted the board).

### Fixed

- A TCP connection to the device lost whatever it sent before the program serving it got round to accepting it. A package's TCP server could miss the start of a request, and an SSH client's opening line was always lost. A connection that was reset while it waited also left a dangling pointer behind. Both are fixed for every TCP server on the device.
- `cat` of a file without a final newline added one into pipes, redirects and host tools as well as on screen, so `cat a > b` and `ssh dev cat f > f` changed the file. It only adds one on a screen now.
- `wifi power`, when the power source had not been read yet (just after the radio came back on), took it to be a battery and could turn power saving on while plugged in. It asks the chip instead.

### Security

- An ordinary account can no longer change anything in `/os` or `/etc` (startup items, services and scheduled tasks, installed packages and their index, the trusted certificates, a staged update: all of it runs as the console's user at the next boot), nor anything in another user's home, and it cannot read another user's `~/.ssh`. Reading is otherwise as before. `/os/ssh`, where the SSH host key lives, is closed to everyone but an admin. `/etc` used to be open to every account, which was a way to root at the next restart; with SSH it would have been one from the network.
- `kill` by an ordinary account stops only its own tasks: from a remote login, that login's; at the console, the ones that follow the console. The SSH server, another person's session and the device's services are an admin's to stop.
- Defining or removing an alias needs an admin now. Aliases are expanded in everyone's shell before any command is looked up, so an ordinary account could put `exit=<anything>` in front of the admin; a built-in alias (`exit`, `id`, `exec`) can no longer be shadowed at all.
- A package run by an ordinary account can no longer write the settings that decide who gets in (`System.Autonomous`, `System.Setup`, `SSH.*`, `Alias.*`), and one run from a remote login only its own (`Apps.*`; not `User.*`, which is the console user's). Before, any package anybody could `run` could set the device to boot straight into root's shell.

### Changed

- Who a command runs as belongs to the task now, not to the console. A remote login's shell, and everything it starts, acts for that login's user, in its own working directory, with its own Ctrl+C; every other task follows whoever is signed in at the console, as before. So a guest logged in over SSH stays a guest while root is at the serial port, and a remote root does not make the console's guest an admin. `logout` in a remote session ends that session and leaves the console alone. Each task carries 160 bytes more for this (about 2 KB in all).
- A full-screen app belongs to the terminal that opened it; one started on a second terminal while another is open is refused instead of drawing into the first.
- The identity and security changes above cost the Pico W 2.8 KB of its slot (15.5 KB left); SSH itself is not built there.

### Known issues

- The filesystem part of `bench` scores 108 against 120 on v2.00-beta (10% slower). v2.00-beta rebuilt from source scored 112 to 117 on the same board, and one firmware varies from 100 to 126 between boots, so part of the gap is the state of the filesystem, but it has not been traced.
- For the first few seconds of traffic after joining a network, pings can average 60 to 80 ms whatever the power setting; the chip reports power saving off throughout, and it settles to about 1 ms.

## v2.02-beta (build 582), 2026-11-20

Summary: A small fix release: the browser tool shows the RPCortex mark instead of its own lettering.

### New

### Improved

### Fixed

- The browser tool (`tools/web`, and the Tool page on rpc.novalabs.app) spelled RPCortex in its own gradient lettering. It shows the RPCortex R mark now, the same one as the website.

### Security

### Changed

### Known issues

- Only the Pico 2 W has run this build. The Pico 2, Pico W and Pico images build and pass the image checks; the Pico image boots in the emulator.
- A saved rollback copy takes most of a Pico 2 W's filesystem; the next update removes it to make room.
- SSH has not been run on a board. The protocol and the device's server code are proven against OpenSSH on a computer; the sockets, the radio and the timings are not.
- An SSH key exchange runs on the console's core, so anyone who can reach the port can keep that core busy by connecting over and over, before logging in. Leave the server off on networks that are not trusted.
- Output printed over SSH while a command holds the filesystem or the network is held back 8 KB at a time and cut beyond that, with a note (`curl` to the screen of a big page, `ls` of a huge directory).

## v2.01-beta (build 578), 2026-11-20

Summary: The first Vela II update: tools to drive a board from a computer, Wi-Fi that answers in about a millisecond, smaller packages, and a long list of fixes found by testing on a real Pico 2 W.

### New

- `link`: a mode for programs on a computer to drive the board over USB. They can run a command and get its output and exit status back, send and fetch files with a SHA-256 check (about 19 KB/s up and 74 KB/s down on a Pico 2 W) and list folders. The protocol is in LINK.md.
- `rpc`, a command-line tool for Linux, macOS and Windows (`tools/rpc`), and a browser tool over Web Serial (`tools/web`). Both run commands, copy files both ways, back up and restore a board, install packages from the repository even when the board has no network, and open a terminal; `rpc` can also flash firmware. They use `link`.
- `wifi power` chooses how hard the radio saves power: `auto` (the default), `off`, `balanced` or `save`. `auto` keeps the radio listening while the board has USB power and saves power on battery, switching by itself.
- Services are supervised. A service that fails is started again after 2 seconds, then 4, 8, 16 and 32, and is given up on after five failures in a row; a minute of normal running clears the count. A service that was killed or finished cleanly is left alone. `service status` shows each one, and every restart is in the log.
- Package API 1.25, `fw_time_set`: a package can set the clock. Nova D1's GPS screen uses it to set the time from the satellites.
- Package API 1.26, `fw_net_scan2`: a Wi-Fi scan with one row per access point, hidden networks included, with the channel and the security it advertises. Nova D1's wardrive uses it to write a WiGLE file.
- Package API 1.27, `fw_nfc_select` and `fw_nfc_exchange`: a package can find a card on a PN532 reader and talk to it. Nova D1's NFC app uses them to dump Mifare Classic and NTAG cards. Not yet run against a reader.
- `subghz tx @<file>` sends the timings in a file, so a saved `.sub` can be replayed.
- Version numbers are now `vMAJOR.ITER` with the build beside them, like `v2.01-beta (build 561)`. Every release takes the next number, and the label (`-beta`, `-rc`, none) says how finished it is. `tools/release.py` cuts a release, and the websites read the release history from `releases/releases.json`.

### Improved

- Wi-Fi replies about a hundred times faster on USB power. A ping from a PC next to the router went from an average of 157 ms (up to 559 ms) to about 1 ms. The radio's power saving was making it doze between beacons; see `wifi power` above.
- 14 KB more free memory on every board. Buffers that sat reserved the whole time the device ran, but were only used by one command (the boot scan, `download`, the USB drive's import, the task and service lists, Tab's list of matches), are now taken only while that command runs.
- Packages are 7 to 19% smaller on disk and to download (Nova D1 went from 440 KB to 365 KB). The build drops symbol names the loader never reads, and keeps the full file beside each one as `<name>.dbg` for debugging.
- Typing at the console sends back what was typed rather than the whole prompt and line again for every key: 130 bytes for a 48 character command, where it used to be 3,604. It matters most on a UART console and for pasted text.
- Boot is about a second faster when no terminal is open (after a reboot, or on battery). It used to wait up to three seconds for one; it now waits for the computer to see the device, then only briefly.
- Loading a big package reads much less of its file (a traced Nova D1 load and install went from 77 MB of reads to 10 MB). On a Pico 2 W an install is about 10% faster, since writing flash is most of it; the RP2040's copy into RAM gains more.
- `wc` takes `-l`, `-w` and `-c`, counts several files with a total, and counts a named file even when a pipe is also feeding it. `capture n wc -l file` in a script now gives a number.
- A freshly flashed board no longer opens its log with "Last restart was the WATCHDOG, something stopped responding". A restart by the boot ROM after a flash is recognised and noted as what it is.
- Usage messages, and `logdump`'s own note about dropped lines, no longer go into the 48-line log, where a few typos could push out the boot and crash entries it is there to keep.

### Fixed

- `;`, `&&` and `||` only worked at the prompt. In scripts, `bg`, services, scheduled tasks and from packages, `a ; b` ran as one command called `a`. They work everywhere now.
- A package command's output can be piped, redirected and captured. `greet | wc` used to count nothing, and a package run by another package or a host tool printed straight to the console.
- A script run with `bg` froze the whole board. The script interpreter used about 5 KB of stack before running a line and a background job had 3 KB. The interpreter's state is on the heap now, and background jobs, services and the scheduler get the same 8 KB stack as the shell.
- `compat` and `pkg certs` froze a Pico 2 W whose radio had not been switched on, until the watchdog restarted it. They say the radio is off instead.
- A file or folder name of 40 characters or more was quietly dropped from a path, so the path meant its parent instead: `touch /tmp/<long name>` made a file called `/tmp`, and `rm -r /home/root/<long name>` would have removed the whole home folder. Long names are kept whole, and a path too long to handle is refused with "Path too long".
- `rm -r` could not remove a folder holding a name of 64 characters or more, and two `rm -r` running at once (one in the background) could interfere. A factory reset had the same name limit.
- Tab completion of a path dropped the folder that was typed: `ls /ho` became `ls home/`, a different path. It keeps it now (`ls /home/`, `ls ../os/`).
- `/tmp` is really emptied at every boot. Folders inside it were skipped and stayed for ever.
- After first-run setup, `/home/root` and `/home/guest` did not exist until the next boot.
- The Pico and Pico W images stopped right after POST: the RP2040 stack guard sat on bytes the scheduler reads at every task switch, and it was set up as a region smaller than that chip allows. Fixed and checked in the emulator; real RP2040 boards are still to be tried.
- `download` only sometimes ended on the first key; most keys were taken by its own Ctrl+C check and came back at the next prompt (a `q` turned into `qls`).
- `compat` warned "0 bytes free on the boot stack" on every board. It was measuring the wrong stack.
- `kill 1` said "It is the shell"; pid 1 is init. Each is now named for what it is.
- `df` no longer reports a failed measurement as a full disk. When the filesystem cannot say how full it is, `df` says so and suggests `fscheck`.
- The boot recovery ladder (start with nothing loaded after three failed starts, then the saved firmware, then a rebuild) could never run on a board, because the failure count was kept in a register the SDK overwrites first. It now works, confirmed on a Pico 2 W along with `update rollback` both ways.
- A build the device rolled back from is now really refused by `update check`.
- Two tasks using the filesystem at once could leave littlefs pointing at freed memory: reading a file in pieces (rollback checks, WAV playback, `fw_file_read_at`) skipped the filesystem lock. Likely the cause of a filesystem that read 100% full until a big file was deleted. Every littlefs call takes the lock now, and the build fails if one ever does not.
- Saving settings while a package changed one on the other core could write stray memory into `/os/registry.cfg`, or mark a setting saved that was not. The same applies to the accounts file and per-user settings.
- A package could read half of one setting and half of another if the setting changed at that moment.
- More races between tasks and cores, each found and fixed: a package command called after its package was unloaded; pipes inside a package's command getting another task's output; two tasks getting the same pid; one command's error clearing another's (so `cp x y && rm x` could remove `x` after a failed copy); the package loader's working buffers, the task and service lists and the scheduler's table shared between tasks; two I2C transfers on one bus at once.
- A task started by a package could run on less stack than its firmware calls need (on RP2040, and on RP2350 for packages built without veneer gates).

### Security

- `rm`, `mv` and `rename` refuse `/` and the system folders (`/os`, `/os/pkg`, `/etc`, `/home`, `/tmp`, `/usb`) themselves, even for root, and say how to empty one instead. `rm /tmp` used to delete it without a word, and nothing stopped `rm -r /os`, which removes every account and package.
- `mkdir` asks the same permission question as every other command that writes; a guest could make folders inside protected ones.

### Changed

- `bg` jobs, services and the scheduler take 8 KB of memory each instead of 3 KB, because they run full shell lines (eight services is 64 KB). A package task that runs outside the sandbox takes what it asks for plus 8 KB. Starting one fails cleanly if memory is short.
- Two `pkg install` at once, or two `download` at once, are refused with a message instead of interfering. `download` only runs at the prompt.
- The update channel compares `MAJOR.ITER.BUILD`, for example `2.01.486`. An older device that compares `2.0.0.483` still updates into it.

### Known issues

- Only the Pico 2 W has run this build. The Pico 2, Pico W and Pico images build and pass the image checks; the Pico image boots in the emulator.
- A saved rollback copy takes most of a Pico 2 W's filesystem; the next update removes it to make room.
- The Pico W has only about 12 KB left in its firmware slot.

## v2.00-beta — "Vela II" (build 483, 2026-10-28)

The rewrite. RPCortex was MicroPython for four versions; this one is C++, and
almost everything underneath is different even though the shell is deliberately
familiar.

It is **in beta**. It boots, joins a network, installs and runs packages, serves
web pages, and updates itself over the air — all confirmed on real hardware. It
is also changing week to week, and the list of what is missing is at the bottom
rather than buried.

This entry is the narrative for the first release of the generation, v2.00-beta, which
was published on GitHub on 2026-10-28 as the tag `v2.0.0`. What changed in each later
release is in the dated sections above it.

---

### The headline

**No interpreter.** At 230 MHz, the clock v1's score of 5,000–6,000 was taken at (v1's
boot-clock setting was not reliable, so it was set by hand), a Pico 2 W scores about
**1.05 million** on RPCMark: roughly 190 times as fast, on the same workload at the same
clock. That is not a typo and it is not a projection — `bench` runs the same workload as
`tools/bench.py` does on v1, same iteration counts, so the two numbers are the same
measurement taken twice. At the 150 MHz v2 starts at it scores about 700,000, and the score
follows the clock almost exactly: 900,000 at 200 MHz, 1.1 million at 250 MHz (`pulse set`,
or `pulse boot` to keep it).

**A bad package costs a command, not the device.** This is the part with no
equivalent in v1 at all. On RP2350 a package runs unprivileged, with the memory
protection unit describing exactly five regions it may touch, and every pointer
it hands the firmware range-checked against those regions. All three ways a
package can misbehave are contained and confirmed on hardware:

| The package… | The cost |
|---|---|
| Uses a bad pointer | The command. A report names it; the shell carries on. |
| Stops responding entirely | The command. A timer takes the call back. |
| Runs out of its own stack | The command. |
| Asks for something that does not exist | Nothing. The call is refused. |

That last row sounds trivial and is not: `havoc` is a package built to attack
the ABI from inside the sandbox, and it runs 62 checks that all have to come
back as clean refusals.

**One image, drag and drop.** No firmware to flash and then files to paste on
top of it. Hold BOOTSEL, drop a `.uf2`, done — and after that the device
updates itself.

---

### What is new that v1 never had

- **Both cores.** v1 could not use core 1 at all; MicroPython's heap is not
  safe across cores. An unpinned task here lives on core 1 about 99% of the
  time.
- **A task manager.** `ps` shows pid, state, core, stack used against
  allocated, CPU time and where each task was started from. `kill` stops one.
  On v1 a misbehaving background service could only be found by rebooting.
- **Crash reporting that survives the crash.** A log ring and a black box in
  memory the reset does not clear, so the next boot says what the last run was
  doing when it stopped. v1 lost everything on reboot. After a bad shutdown the
  next boot also writes the black-box record and the log to `/os/crash.log`, so a
  crash survives the power being pulled as well.
- **A USB drive.** `download` presents a real FAT volume, so files move by drag
  and drop in both directions instead of over the serial line.
- **Bluetooth.** The same chip does both radios. v1 could not reach Classic at
  all — MicroPython's module is BLE only — so anything that plays audio was out
  of reach. `bt` scans, advertises and names what answers, on LE and Classic;
  `btaudio` is an A2DP source, reading a WAV off flash and playing it to a
  speaker. A2DP is four layers deep — L2CAP, AVDTP, A2DP, then SBC — which is
  why it sits in the firmware rather than in a package.
- **A runaway task is a killed task.** A task that holds a core past its 250 ms
  slice is taken off it by a timer. Scheduling stays cooperative by default, so
  the reasoning about shared state is unchanged; preemption is the safety net,
  not the normal path. On v1 a `while True:` was a reboot.
- **A memory card.** `sd` mounts one at `/sd`, and it is a second volume rather
  than a second set of commands — `ls`, `cp`, `tree` and a script all reach it
  through the same paths. Read-only FAT12/16/32, RP2350 boards only, and it has
  not yet been run against a real card.
- **The parts a handheld is made of.** `nfc` and `ibutton` for the two contact
  readers, `subghz` for a CC1101 and `lora` for an SX1276. They are firmware
  rather than packages because those buses are shared, and arbitration is not
  something one package can do on behalf of the rest of the system.
- **Settings that belong to a person.** `User.`-prefixed registry keys are
  per-account, so two people on one device do not overwrite each other.
- **A package can run a shell command** and read back what it printed
  (`fw_shell_run`) — but not from inside another package command, which used to
  overwrite the outer call's way home and is now refused.
- **A package can run from flash rather than from RAM.** The read-only half of a
  position-independent package — code, constants and the veneers into the
  firmware — holds no absolute address, so it is written to a flash slot once and
  executed where it lies. Only the writable half is resident: Nova D1 goes from
  174 KB to 62 KB. RP2350 boards only, and the parts of it a host cannot reach —
  the chip, fetching instructions out of XIP flash, and handing the memory
  protection unit a flash base — are listed as unconfirmed in `pkgslot.h` and
  have not been answered by a board. A slot reaches the firmware by index, so a
  firmware update that reordered or dropped an ABI symbol would leave those
  indices naming the wrong functions — the one corruption that does not fault, it
  just makes a package's own subcommands stop matching. The slot now records a
  fingerprint of the ABI table it was built against; a boot whose table no longer
  matches refuses the slot and loads that package from its `.app` instead, so an
  over-the-air update can never quietly break a slot-resident package. Append-only
  additions leave the fingerprint intact, so an ordinary update keeps the slot.
- **A Nova D1 app is one text file.** A `.napp` in `/nova/apps` is a short header
  and a dozen rows of `Label = shell command`. Nothing runs when one is
  installed, and nothing is executed afterwards either — the file is read. That
  is the whole security story, and it is also forced: one package call per task,
  and a Nova D1 screen is already inside one, so a screen cannot call into
  somebody else's package. What an app reaches instead is the firmware command
  surface — the radios, the contact readers, the network, the filesystem. What
  it cannot reach is the tools that happen to be packaged: `gpio`, `i2cscan`,
  `dht`, `ws2812`. A faulty app is listed with the reason it will not open
  rather than quietly dropped.
- **Protected files.** The registry, the user list, their backups and the device
  key are closed to every account except an admin: not readable, not changeable,
  and not reachable through a directory or a wildcard (`rm -r /os`, `rm /os/*`).
  That covers the shell commands, redirects, the editor, the USB drive and the
  file calls a package makes, so a guest cannot read saved secrets or edit the
  user list into making themselves an admin. v1 had no such protection.
- **Real background work.** v1's `task run` entered a foreground scheduler: it
  was scheduled tasks or an interactive shell, not both. Here the timer is just
  another task.
- **Interruptible everything.** Ctrl+C stops any command instantly, because the
  interrupt check and the scheduler yield are the same call.
- **The package loader refuses a damaged or hostile package instead of trusting it.**
  Fuzzing it found four faults, all in how it believed numbers read from the file: a
  relocation offset past the end of its section was a write wherever it pointed, and
  an odd one a misaligned read that faults on RP2040; a section-name offset past the
  end of the name table was a read from wherever it landed; and a section size or
  alignment that wrapped the 32-bit layout total allocated a small block and then
  filled it at the claimed size. Each is now a refusal with a reason. The packages
  `build.sh` produces load exactly as before.
- **Pastes and passwords arrive whole.** Only the shell's own task reads the
  console, so a background package polling for a stop request cannot take
  keystrokes meant for the prompt (it used to: pasted lines lost characters and a
  password typed at the login prompt arrived short). Anything typed while a
  command runs waits in a 512-byte stash, then in the USB buffer, which holds
  the rest back from the host when it fills. The login prompt and the line editor
  both read the stash first.
- **PIO, SPI and I2C for packages**, which is what makes drivers possible
  without touching the firmware.
- **It puts itself back together.** Settings and accounts are written twice, so
  a file that stops reading is restored from the copy beside it rather than
  lost. Every file the OS owns is read at boot, because a bad flash block on
  littlefs does not return wrong data — it returns nothing at all — and one that
  cannot be read is removed and rebuilt. `fscheck --scan` reads the rest.
- **Firmware rollback.** An update saves the version it replaces, and `update
  rollback` writes it back. Files and settings are untouched; only the firmware
  changes.
- **A device that recovers on its own.** Three starts that never reach a shell
  and it tries the cheapest fix first: load nothing — no packages, services or
  startup items, which is what breaks a working device most often. Still
  failing, it restores the saved firmware. Only when there is nothing left to
  restore does it rebuild the filesystem, which is the step that actually costs
  something. v1 had none of this: a device that would not boot needed another
  computer. Confirmed on a Pico 2 W (`failboot 2` and `failboot 3` stand in for the
  failed starts): the third start loads nothing, and the fourth verifies the saved
  image, writes it back and comes up on the older build. Getting there found that
  the ladder had never been reachable: the start count lived in a watchdog scratch
  register that the SDK's own watchdog setup overwrites before the count is read,
  so every start counted as the first. It is kept in a register nothing else
  touches now. A restart that is not a failure (a hang the watchdog ended, a
  crash) counts once and is cleared when the shell comes up.

### What matches v1

The filesystem commands down to `ls`'s exact columns. Text processing. Accounts
with salted SHA-256, roles and the guided first run. The shell: pipes, `&&`,
`||`, `;`, redirection, quoting (single and double; a quote opens only at the start
of a word, so `wifi connect Dad's WiFi` and `echo don't` keep their apostrophes),
history, the aliases. `wifi` with all its
subcommands, `ping`, `nslookup`, `ntp`. `startup`, `task`, `service`, `watch`.
The `.rps` scripting language, unchanged — scripts carry over as they are. Idle auto-logout
(`Shell.Idle_Logout`, minutes, off by default). `curl` takes all of v1's flags: `-X`, `-d`, `-H`,
`-I`, `-v`, `-s`, `-o` and `--timeout`. `pulse boot <MHz>` sets the clock the device
starts at, and it does apply it: a start that follows one which did not finish skips
it and says so, so a clock that hangs the board cannot do it twice running.
`pulse boot off` turns it off.

And the console itself: v1's tagged output, colours and boot banner, checked
byte for byte against the original escape sequences.

---

### Boards

| Board | Image | Filesystem | Sandbox | Card |
|---|---|---|---|---|
| Pico 2 W | 855 KB | 1.75 MB | yes | yes |
| Pico 2 | 352 KB | 2.75 MB | yes | yes |
| Pico W | 805 KB | 384 KB | no | no |
| Pico | 297 KB | 1.25 MB | no | no |

The image figures are the shipped `.bin` sizes, and the development build is the
largest one gets, so it is what every flash slot is sized against; the table they come from is
in `os/CMakeLists.txt`, next to the slot sizes it justifies. "Card" is a microSD
slot, which is built for the RP2350 boards only.

The RP2350 images build the scheduler, the sandbox and the package-call path at
-O3, and everything that waits on a serial line, a radio or a flash write (the
shell commands, the Bluetooth, TLS and TCP/IP stacks, littlefs and the USB and
FAT code) at -Os. That took the Pico 2 W image from 1009 KB to 855 KB, leaving
169 KB of its 1024 KB slot free; `-DRPC_COLD_SIZE=OFF` builds the old way to
compare. The RP2040 images were already built at -Os.

**The RP2040 images have not been booted.** They build, both image checks pass
on them, and the flash layout was verified by reading the constant back out of
the compiled firmware — but no Pico or Pico W has run one. Every hardware
result quoted above is from a Pico 2 W.

The RP2040 boards are back. v1.0 had to drop them because the multitasking
build did not fit in 264 KB of RAM; without an interpreter it does. They have no
package sandbox — ARMv6-M protection regions are power-of-two sized and aligned
to their own size, so the five a package needs cost more RAM than those boards
have — and packages there run with the OS's own privileges.

---

### Known limitations

- **HTTPS needs about 45 KB of free memory.** A handshake holds the session, the
  certificate chain and the key exchange in the heap. Measured on a Pico 2 W: 41 KB
  free failed and 47 KB worked. A board with Nova D1 and every developer package
  loaded used to boot with 41 KB; it now boots with 69 KB. A handshake that runs out
  says so and gives the numbers, and `unload` or a restart brings the room back.
- **A Ctrl+C behind a full stash of type-ahead waits.** Input that arrives while a
  command runs is kept, up to about 500 bytes beyond the USB buffer; past that the
  check stops reading, so a Ctrl+C typed after a very large paste is seen when the
  command ends. Nothing typed is dropped to make room.
- **A rollback copy takes most of the filesystem.** `update install` saves the running
  firmware before it applies the new one (seen on a Pico 2 W: "Rollback copy saved:
  v2.0.0 (854 KB)"), and that copy is a file of 854 KB on a 1792 KB filesystem. The next
  update over the air removes it to make room and saves the running one again.
  `update from-file` leaves the file it was given where it is, so there is not room for
  both, and it says so and carries on without a rollback copy. Restoring a saved copy
  (`update rollback --now`, and the automatic restore after failed starts) has been run on a
  Pico 2 W in both directions, 483 to 481 by hand and 486 to 481 by the ladder; the restored
  image is the one that was saved, checked against its recorded SHA-256 before it is written.
- **A filesystem that reads 100% full with a saved firmware copy on it.** Seen twice on a
  Pico 2 W, once after `update from-file` and once after an update over the air: `df`
  reported 0 KB free of 1792 KB with about 1.3 MB of files on it, every write was refused
  ("Could not write"), and `fscheck` found nothing wrong. Removing the large saved image
  (`rm /os/rollback.img`, or `rm` of the file given to `update from-file`) gave back
  1164 KB at once. It has not been reproduced since (75 hours of uptime on the next build
  and a second update over the air were normal), and the cause is not known. The updater
  now drops the saved copy and measures the room again when a download does not fit.
- **Saved Wi-Fi passwords are sealed, not unbreakable.** They are stored
  encrypted under a per-device key (`/os/device.key`, left out of backups), so a
  copy of the registry, a backup or a support dump no longer shows them, and an
  ordinary account cannot read the key or the registry (see "Protected files").
  The key is in the same flash as the passwords, so someone holding the whole
  device can still recover one: there is no secure element to put it in.
- **No package sandbox on RP2040.** Not an omission — ARMv6-M protection regions
  are power-of-two sized and aligned to their own size, so the five a package
  needs cost more RAM than those boards have. Packages there run with the OS's
  own privileges. `mpu` says which a board is doing.
- **The RP2040 images have never been booted.** As above: they build and they
  fit, and that is all anyone can say for them.
- **The card driver has never seen a card.** The command sequence that brings
  one up is host-tested against a fake card written from the specification, and
  the filesystem against volumes `fsck.fat` approves of. The electrical and
  timing layer is unproven and the source says so.
- **A task inside a lock or a flash write cannot be taken off its core.**
  Everything else can, at 250 ms. The exception is the case where interrupting
  costs more than waiting.
- **The wireless driver's lock is keyed on the core, and the OS works around
  it rather than fixing it.** cyw43's own mutex treats two tasks on one core as
  the same owner and makes a task on the other core wait in a `__wfe` with no
  timeout. Everything that reaches the radio therefore goes through one
  operation lock which migrates the caller to the core the chip came up on —
  `os/shell/netown.h` states the rule and every entry checks it, with `wifi`
  reporting a count of any that got through without it. The count is zero. The
  underlying SDK behaviour is unchanged and a new call site added carelessly
  can still reintroduce it.
- **`put` needs a sender that waits.** The device acknowledges each 256 bytes
  once they are on flash; `tools/putfile.py` waits for that and verifies a
  sha256, and is the only sender worth using. Bytes pasted by hand still work
  and are still unverified.
- **Not every v1 package has been ported.** The ones that have are in
  `repo-v2/index.json`; the rest are transcription rather than design, but they
  are still the distance left to parity.
- **No ESP32-S3.** v1 runs there; this does not yet. The portable core moves
  unchanged, but the context switch, storage and network layers do not.
- **Beta.** If something does not match the device, the device is right.

---

### Upgrading from v1

v2 replaces the firmware entirely, so installing it removes MicroPython and
everything stored under it. Anything worth keeping has to come off first —
`download` on v1, or whatever route is already in use.

Scripts carry over. Packages do not: a v1 `.pkg` is Python source and there is
no Python here.
