# The browser tool

A page that connects to an RPCortex v2 device over USB: a terminal on its
console, a file browser, a command box, package installs and the device's own
reports. Static files with no build step, so it runs from `rpc web` on a
computer or from any https site.

## Opening it

```
rpc web                          # serves this folder on http://localhost:8765 and opens it
rpc web --port 9000 --no-browser
```

Any static server works as long as the address is `http://localhost` or
`https://`: Web Serial is offered only to secure pages, and a page opened as a
`file://` path cannot load its own modules either.

Add `?demo` to the address (`http://localhost:8765/?demo`) to try the page
against a pretend device in the page itself, with no board attached.

## Which browsers

Chrome and Edge on a desktop computer (Windows, macOS, Linux, ChromeOS), and
other browsers built on Chromium. The page talks to the board through the
[Web Serial API](https://developer.mozilla.org/docs/Web/API/Web_Serial_API),
which Firefox and Safari do not implement. Phones and tablets have not been
tried. On a browser without it the page says so, and the command-line tool
(`tools/rpc`) does the same jobs from a terminal.

The browser asks which port to use the first time. The list shows Raspberry Pi
boards only; "List every serial port" shows the rest. On Linux the account
needs to be in the `dialout` group (see `tools/rpc/README.md`). A port can be
open in one program at a time, so close any other terminal on it first.

## What is on it

| Tab | What it does |
|---|---|
| Connect | Choose the device, log in if it asks, see background messages, reset to BOOTSEL for a firmware update. |
| Terminal | The console, keys straight through. xterm.js when it can be loaded, a plain terminal when it cannot. |
| Files | Browse folders, upload by picker or by dropping files on the list, download, rename, delete, make folders. Every transfer is SHA-256 checked. |
| Run | Run a command line through the device's shell and see its output and exit status. Stop sends Ctrl+C. |
| Packages | What is installed, install a `.app` file, or install from the repository: the index and the package are downloaded in the browser and checked against the index's SHA-256 before anything is sent, so the device needs no network. |
| Device | Firmware, board and account, memory, storage, tasks, the log, and a restart. |

Terminal and the other tabs share one port. The terminal shows the console as
a person sees it; Files, Run, Packages and Device use link mode
([LINK.md](../../LINK.md)), where every answer is framed and checked. Moving
between them switches the device over, and the badge at the top right says
which it is in. Leaving the page closes the port, and the device returns to its
prompt by itself.

Firmware older than link mode gets the terminal only; the badge says so.

The Reset to BOOTSEL button opens the port at 1200 baud, which makes the board
restart into its bootloader as a USB drive. Copy a `.uf2` onto that drive to
update the firmware. Files on the device are kept.

## The files

```
index.html         the page
css/style.css      light and dark, from prefers-color-scheme
js/link.js         the protocol client; no DOM, also runs in Node and Deno
js/repo.js         the package index: architecture rules and verification
js/serial.js       one Web Serial port shared by the terminal and link mode
js/terminal.js     xterm.js 5.5.0 from jsDelivr (pinned, with integrity hashes) or the plain fallback
js/app.js          the page itself
js/mockdev.js      a pretend device, for the tests and for ?demo
js/demo.js         a pretend Web Serial port in front of it
test/link.test.js  the protocol client and repository rules against the pretend device
```

## Tests

```
node tools/web/test/link.test.js       # Node 20 or later
deno run tools/web/test/link.test.js
```

No dependencies and no browser. They cover entering link mode from the prompt,
from a login prompt and from a device an earlier client left in link mode;
old firmware; EXEC with chaining and a timeout; PUT and GET at every size
around the chunk with a Unicode path; error codes; a damaged GET caught by the
digest; cancelling an upload; requests issued together running one at a time;
installing a package; and the repository's architecture and checksum rules.
