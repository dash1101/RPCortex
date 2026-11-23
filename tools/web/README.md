# The browser tool

A page that connects to an RPCortex v2 device over USB: a terminal on its
console, a file browser, a command box, packages, firmware updates and the
device's own reports. Static files with no build step, so it runs from
`rpc web` on a computer or from any https site.

## Opening it

```
rpc web                          # serves this folder on http://localhost:8765 and opens it
rpc web --port 9000 --no-browser
```

Any static server works as long as the address is `http://localhost` or
`https://`: Web Serial is offered only to secure pages, and a page opened as a
`file://` path cannot load its own modules either.

Add `?demo` to the address (`http://localhost:8765/?demo`) to try the page
against a pretend device in the page itself, with no board attached. Its account's
password, which sudo asks for, is `demo`. It runs v2.01-beta, so the Firmware tab
shows a device that needs `--force` once, and its update and restart play out the
way a board's do.

`?releases=<url>` reads the release files from somewhere other than the
repository's `releases/` folder on GitHub, for trying a release before it is
pushed (a folder served next to the page, say `?releases=/releases/`).

## sudo

On firmware with link protocol 2, installing a package, changing anything in
`/os` or `/etc`, and reading the account file or the keys need sudo
(PERMISSIONS.md). The first time the device says so, the
page asks for the signed-in account's password, sends it with `AUTH` (LINK.md) and
does the request again. A wrong password is asked for again, three times at most.
The password is kept for as long as the page is connected, so a long job is not
stopped by the device's five minutes running out. Older firmware is never asked.

The question is a masked password field, and it is cleared as soon as it is
answered. A long job (Update all, a firmware update) asks before it starts rather
than half way through. Writing firmware from the page is the one place the
password is typed at the device's own prompt (see Firmware below); it is held for
that one update and then dropped.

## Which browsers

Chrome and Edge on a desktop computer (Windows, macOS, Linux, ChromeOS), and
other browsers built on Chromium. The page talks to the board through the
[Web Serial API](https://developer.mozilla.org/docs/Web/API/Web_Serial_API),
which Firefox and Safari do not implement, and which no phone or tablet browser
has. On a browser without it the page says so; the Firmware tab still shows the
latest release, its notes and the board status there, and saves the images. The
command-line tool (`rpc`, from [RPCortex Studio](https://github.com/dash1101/rpcortex-studio)) does the rest from a terminal. The page itself
fits a phone's width.

If another program has the port open (a terminal, Thonny, `rpc term`, the page in
another tab), the browser cannot open it, and the page says that rather than the
browser's "Failed to open serial port".

The browser asks which port to use the first time. The list shows Raspberry Pi
boards only; "List every serial port" shows the rest. On Linux the account
needs to be in the `dialout` group (see RPCortex Studio's README). A port can be
open in one program at a time, so close any other terminal on it first.

## What is on it

| Tab | What it does |
|---|---|
| Connect | Choose the device, log in if it asks, see background messages, reset to BOOTSEL. |
| Terminal | The console, keys straight through. xterm.js when it can be loaded, a plain terminal when it cannot. |
| Files | Browse folders, upload by picker or by dropping files on the list, download, rename, delete, make folders. Every transfer is SHA-256 checked. |
| Run | Run a command line through the device's shell and see its output and exit status. Stop sends Ctrl+C. |
| Packages | What is installed against the repository, with Update all, Update, Remove and Install; each package's architecture and ABI; a `.app` file from this computer. |
| Firmware | The device's version against the latest release, the release notes and which boards have run it, and three ways to update. |
| Device | Firmware, board and account, memory, storage, tasks, the log, and a restart. |

Terminal and the other tabs share one port. The terminal shows the console as
a person sees it; Files, Run, Packages and Device use link mode
([LINK.md](../../LINK.md)), where every answer is framed and checked. Moving
between them switches the device over, and the badge at the top right says
which it is in. Leaving the page closes the port, and the device returns to its
prompt by itself.

Firmware older than link mode gets the terminal only; the badge says so.

A failure is shown at the top of the tab it happened on, and stays until it is
dismissed. The tabs move with the arrow keys, Home and End. The button at the top
right picks the colours: the system's, light or dark.

## Packages

The installed list comes from `_pkgs`, the device's machine-readable list, and is
compared with the v2 index (`repo-v2/index.json` in the RPCortex-repo repository)
by the device's own version comparison, so the page offers exactly the upgrades
`pkg upgrade` would. Every package is downloaded here, checked against the index's
SHA-256, copied to `/tmp` and installed with `sudo pkg install <file>`, so a board
with no Wi-Fi is kept up to date the same way.

- **Update all** asks for the password once, first, then updates each package in
  turn. One that fails is reported and the rest still go.
- **Architecture.** `armv8m` packages run only on an RP2350 (Pico 2, Pico 2 W),
  and the page offers them only there; `armv6m` ones run on every board. The board
  says which it runs in HELLO's `arch`.
- **ABI.** Shown for each package. The device refuses one built for a newer ABI
  than its firmware has, and the page shows the device's refusal as it said it.

## Firmware

The Firmware tab reads three files from the repository's `releases/` folder on
GitHub (`raw.githubusercontent.com`, which allows a page to read them):
`latest.json`, the manifest the device itself reads for `update check`;
`releases.json`, the history and the notes; and `boards.json`, which boards have
run which build. It compares the device's version with the latest one the way
the device does (`repo_version_cmp`, mirrored in `js/version.js`), and offers
three ways to update. Files and settings are kept by all three.

- **Over the air**, on a Pico W or Pico 2 W with Wi-Fi connected. The page runs
  `sudo update online --yes` through link mode. The device downloads the image,
  checks it against the release, writes it and restarts; it prints nothing while
  it works, so the page shows each step and how long it has taken, waits for the
  board to come back, and says what it runs now. `--yes` is there because link
  mode cannot answer the device's "Continue?".
- **From this computer**, on any board. The page downloads the `.bin`, checks its
  SHA-256 against `latest.json` and that it is built for this board's chip, sends
  it to `/os/update.bin` (where `update` itself stages a download, so the update
  removes it before it saves a copy of the running firmware), then types
  `sudo update from-file /os/update.bin` at the device's prompt and answers its
  password and its yes/no question, showing the device's own output as it goes.
  That step happens at the prompt because `update from-file` always asks that
  question and link mode cannot answer one. When there is no room for the image
  beside the saved copy of the previous firmware, the page asks before removing
  it with `sudo update rollback --forget`. "Choose a .bin" sends an image of your
  own instead, unverified.
- **BOOTSEL**, on any board, even one that no longer starts. Where the browser can
  write to a drive (the File System Access API, in Chrome and Edge), the page
  resets the board to BOOTSEL, asks for the drive named `RP2350` or `RPI-RP2`,
  checks from its `INFO_UF2.TXT` that it is the right chip, and writes the `.uf2`
  onto it. Everywhere else, and whenever that does not work, "Save the .uf2"
  downloads it for dragging onto the drive by hand.

**Moving from v2.01 to v2.05.** Those releases numbered their versions
`vMAJOR.ITER`, so a device on one compares `2.05.635` with `2.0.640` and sees the
new release as older. The page spots that case (an old-style version with a lower
build than the release), says so in one sentence, and updates with `--force`.
After that one update, updates are offered as usual.

## The network bridge

A board with no network of its own (a Pico, a Pico 2, or a Pico 2 W built
without Wi-Fi, on firmware with link protocol 3) can fetch through this page
while it is connected. In the Terminal tab, `sudo pkg install greet`, `pkg
update`, `update check`, `wget` and `curl` then work as they would over Wi-Fi:
the device asks, the page fetches the URL with the browser's own `fetch()` and
sends the body back over the cable, one acknowledged chunk at a time. The same
goes for commands run from the other tabs through link mode. The bridge's
traffic stays off the screen, keys typed during a download are sent once it
ends, and Ctrl+C stops it. Each request is a line in the Messages box.

A web page can read only what a server lets it (CORS), so the page bridges less
than `rpc` does:

- **Works:** `raw.githubusercontent.com`, which allows any page to read it. That
  is the package repository, every package in it, and the update manifest and
  the images it points at, so `pkg install`, `pkg update`, `update check` and
  `update install` all work.
- **Does not:** GitHub release downloads (`github.com/.../releases/download/...`):
  neither the redirect nor the file it leads to allows a page to read it. Nor do
  most other sites. The device then says the browser could not fetch it, and
  `rpc term` can.
- **Only GET and HEAD**, with none of the device's own headers: anything else
  makes the browser ask the server first (a preflight), which
  raw.githubusercontent.com refuses. `curl -X POST` or `curl -H` needs `rpc term`.
- **No length up front:** the browser undoes gzip itself, so the device is told
  the body ends when it ends rather than how long it is.

Only `http` and `https` are fetched, never this computer itself (`localhost`,
`127.x`, `::1`) or a link-local address, and a redirect that ends anywhere else is
not passed on. (`Bridge` in `js/link.js` also takes an allowlist and a size cap, 8 MB
by default; the page uses neither list nor a smaller cap today.)

## The files

```
index.html         the page
css/style.css      light and dark, from prefers-color-scheme
js/link.js         the protocol client; no DOM, also runs in Node and Deno
js/version.js      the device's version comparison, and where a device stands against a release
js/updates.js      the Firmware tab: release files, the three ways to update, then the pane
js/pkgs.js         the Packages tab: installed against the index, upgrades, then the pane
js/repo.js         the package index: architecture rules and verification
js/serial.js       one Web Serial port shared by the terminal and link mode
js/terminal.js     xterm.js 5.5.0 from jsDelivr (pinned, with integrity hashes) or the plain fallback
js/app.js          the page itself
js/mockdev.js      a pretend device, for the tests and for ?demo
js/demo.js         a pretend Web Serial port in front of it
test/*.test.js     the tests (below); test/harness.js is their few lines of runner
```

## Tests

```
for t in tools/web/test/*.test.js; do node "$t"; done      # Node 20 or later
for t in tools/web/test/*.test.js; do deno run --allow-net=127.0.0.1 "$t"; done
```

No dependencies and no browser. `os/host/run_all.sh` runs every file here.

- `link.test.js`: entering link mode from the prompt, from a login prompt and
  from a device an earlier client left in link mode; old firmware; EXEC with
  chaining and a timeout; PUT and GET at every size around the chunk with a
  Unicode path; error codes; a damaged GET caught by the digest; cancelling an
  upload; requests issued together running one at a time; installing a package;
  and the repository's architecture and checksum rules.
- `version.test.js`: every case in `os/host/repoindex_test.cpp`, the device's
  version string from each era's HELLO, and where a device stands against a
  release, including the v2.01 to v2.05 case and v2.00-beta, which needs no force.
- `updates.test.js`: the release files; checksums; which chip an image or a
  BOOTSEL drive is for; writing a `.uf2` to a pretend drive; over the air (sudo
  once, `--yes`, the restart, the version after it, the force case, no radio, no
  network, a failed download, a wrong password); from the page (the PUT to
  `/os/update.bin`, making room, `from-file` at the prompt, an image the device
  refuses); and why EXEC cannot be used for either question.
- `pkgs.test.js`: reading what is installed, the plan against the index, Update
  all with a wrong password and a lapsed grace, a bad download among good ones, an
  RP2040 never being sent an `armv8m` package, and removing.
- `bridge.test.js`: the network bridge against a pretend device with no network
  and an HTTP server on 127.0.0.1 (the only network it needs, hence
  `--allow-net=127.0.0.1` under Deno): the offer, `wget` and `curl` mid-EXEC, `sudo
  pkg install` by name, the allowlist, the size cap, the device stopping part
  way, one chunk in flight at a time, the browser's limits (GET and HEAD, http
  and https, a CORS failure said plainly), and the terminal: frames kept off the
  screen, keys held during a download, and a stray answer never reaching the
  prompt.

The pretend device (`js/mockdev.js`) speaks link protocol 2 with `AUTH` and
`ERR sudo` (protocol 3 and the bridge with `netless`), asks for the password at its own prompt, forgets the grace when link
mode ends, runs `update` and `pkg` the way the firmware does, refuses a PUT that
would fill it, and restarts: its port drops and comes back with the new version.
