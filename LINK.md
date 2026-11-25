# The link protocol

How a program on a computer drives an RPCortex v2 device over its serial console:
run a command and get its output and exit status back, send a file, fetch a file,
list a directory. It is what [RPCortex Studio](https://github.com/dash1101/rpcortex-studio) (`rpc` and `rpc tui`) and `tools/web`
(the browser tool) speak.

The console is built for a person: a line editor that echoes and repaints, colour,
prompts, suggestions. A program reading that has to guess where a command's output
starts and ends. Link mode is the same console with all of that switched off and
every answer framed, so nothing has to be guessed.

Protocol version: **3**. Version 2 added `AUTH` and the `sudo` error, for firmware
where installing a package or changing `/os` or `/etc` needs sudo (PERMISSIONS.md).
Version 3 added the network bridge (below): a board with no network of its own asks
the host to fetch a URL for it. A host reads `proto` from `HELLO` and does not send
`AUTH` to a device that says 1, or `BRIDGE` to one that says less than 3.

## Entering and leaving

Link mode is a shell command, `link`. It runs as whoever is logged in, with that
account's permissions, exactly as if each request had been typed at the prompt.

A host enters it from a prompt:

1. Send `0x03` (Ctrl+C) and wait about 200 ms. That abandons anything half-typed.
2. Look at what came back. If nothing did, send an empty line (`\r`) and look
   again. A login prompt ignores Ctrl+C and refuses an empty answer without
   counting it as a try.
3. Only at a shell prompt (`user@host:/path>`), send `link\r`. At a login prompt
   (`Username`) nobody is logged in: log in first, or ask the user to. Never type
   `link` at a login prompt.
4. Read until a `HELLO` frame arrives (below). Everything before it is the echo
   of the command and a one-line notice for a person; ignore it.

If the device answers `'link' is not a command`, the firmware is older than link
mode; see "Older firmware" below. A device found already in link mode (a tool
that crashed and left it there) answers a `HELLO` request with a frame.

It leaves link mode, sends a `BYE` frame (`0x1E BYE`), and the normal prompt
comes back, when:

- the host sends `BYE`;
- a `0x03` byte arrives while it is waiting for a request (so a person who typed
  `link` by accident gets out with Ctrl+C);
- the USB host stops asserting DTR (the port was closed) for 3 seconds.

## Framing

**Host to device:** one request per line, ending `\n` (a `\r` before it is
ignored). UTF-8; paths are passed through byte for byte. At most 1100 bytes,
counted in bytes, including the newline. The first word is the
verb; words are separated by one space. Where a request ends in a path or a
command line, that last field is the whole rest of the line, spaces included.

**Device to host:** one frame per line. **A frame starts with the byte `0x1E`**
(ASCII record separator), then the frame type, then optional fields, then `\n`.

```
0x1E TYPE [field ...] \n
```

Frames may end in `\r\n`. A frame can also follow part of a background line on
the same line, so look for `0x1E` anywhere in a line, not only at its start.

Any line that does not start with `0x1E` is not part of the protocol. Background
tasks (a service, the scheduler, a log line from the network) can still print
while link mode runs, and their lines can appear between frames. A host should
pass them on to the user or log them, never parse them. Frames are written whole
under the output lock, so a background line never lands in the middle of one.

There is no echo, no colour and no prompt in link mode.

**Every request gets exactly one final frame**, `OK` or `ERR`, after any
intermediate frames. A host sends one request, reads frames until the final one,
and only then sends the next. Requests are not pipelined.

```
0x1E OK [key=value ...]
0x1E ERR <code> <message to show a person>
```

Error codes:

| code | meaning |
|---|---|
| `badreq` | unknown verb, missing field, line too long |
| `nofile` | path does not exist (or is the wrong kind) |
| `perm` | this account may not read or change that path |
| `nospace` | the filesystem cannot hold it |
| `digest` | a transfer's SHA-256 did not match; nothing was kept |
| `toolong` | the path is longer than the device can resolve |
| `busy` | another transfer holds the console, or out of memory |
| `timeout` | the host went quiet in the middle of a transfer |
| `cancelled` | the host sent `CANCEL` |
| `io` | the filesystem refused a read or write |
| `sudo` | the request needs sudo and the session has no grace: send `AUTH`, then the request again (protocol 2) |
| `auth` | `AUTH` was given the wrong password, or the account has none (protocol 2) |
| `nobridge` | `BRIDGE on` to an image that has a network of its own (protocol 3) |

Binary data travels as **standard base64** (RFC 4648 alphabet, `=` padding) in
`D` lines and frames. Each `D` carries at most the `chunk` number of bytes from
`HELLO` (768 in protocol 1, which is 1024 base64 characters).

## Requests

### HELLO

Sent by the device unprompted on entering, and again on request.

```
> HELLO
< 0x1E HELLO proto=3 os=v2.0.640-beta build=640 board=pico arch=armv6m user=root cwd=/ chunk=768 exec_max=32768 bridge=1
< 0x1E OK
```

On entry the `HELLO` frame comes on its own, with no `OK` after it, because there
was no request. `exec_max` is the most output one `EXEC` returns. `bridge=1` (protocol
3) says the image has no network of its own and can use the host's (see "The network
bridge"); an image with Wi-Fi says `bridge=0`. `arch` is the package
architecture the board runs (`armv8m` on RP2350, `armv6m` on RP2040): a host
installing a package picks the build for it, since the device does not check.
`cwd` is
percent-encoded (a space is `%20`, a percent sign `%25`) so that every field is
one `key=value` word. Fields may be added in later versions; a host ignores keys
it does not know.

### PING

```
> PING
< 0x1E OK
```

### EXEC <command line>

Runs the line through the shell's own runner: pipes, `&&`, `;`, redirection and
aliases all work, and the same permission checks apply. Output is captured,
including status lines such as `[!] ...` (without colour), and returned when the
command finishes.

```
> EXEC ls /home
< 0x1E OUT ICBUWVBFICAgU0laRSAgIC4uLgo=
< 0x1E OUT ...
< 0x1E OK rc=0 truncated=0
```

- `OUT` frames carry the output in base64, in order.
- `rc` is 0 when the command succeeded and 1 when it reported an error.
- `truncated=1` means the output passed `exec_max` and the rest was dropped.
- Output arrives when the command has finished, not as it runs.
- While a command runs, the host must not send anything except a single `0x03`
  byte, which interrupts it the way Ctrl+C does at the prompt, and (protocol 3,
  once the host has offered the bridge) the lines that answer a `FETCH` frame.
  `FETCH` and the frames that go with it can arrive between the `EXEC` and its
  `OUT` frames; see "The network bridge".
- An interrupted command ends `OK rc=1`, with `[?] Interrupted.` in its output.
- A command that stops to ask a question (a yes/no confirmation) waits for keys
  the host is not sending. Use the terminal for those, or send `0x03` to give up.
- sudo never asks for a password here: the keys come from a program. A command
  with `sudo` in front runs while the session has a grace (`AUTH` starts one).
  Without one, that command fails as it would with no password, the rest of the
  line goes on as it would after any failure (`&&` stops, `;` does not), and the
  final frame is `ERR sudo` instead of `OK`, after the `OUT` frames. Send `AUTH`,
  then the same `EXEC` again. Whatever else the line did has then run twice, so a
  host sends one command per `EXEC` when it may need sudo.
- A command that changes the system and has no `sudo` in front is refused as it
  would be at the prompt: `OK rc=1`, with `'pkg install' changes the system. Run
  it with sudo: ...` in its output. Put `sudo` in front.

### PUT <size> <sha256> <path>

Writes a file. `sha256` is 64 hex characters of the whole file's digest.

```
> PUT 1436 9f0e...c2 /tmp/greet.app
< 0x1E READY chunk=768 window=2
> D <base64 of bytes 0..767>
> D <base64 of bytes 768..1435>
< 0x1E ACK 768
< 0x1E ACK 1436
< 0x1E OK sha=9f0e...c2
```

- The device checks the path, the permission and the free space before `READY`,
  and answers `ERR` instead if any of them fails.
- After each `D` line it writes the bytes and answers `ACK <bytes so far>`.
- `window` (firmware that sends it) is how many `D` lines may be sent before the
  first is acknowledged. With 2, the next line is already waiting while the device
  writes this one, so the round trip stops adding to every chunk (380 KB: 10.2 s
  at one at a time, 7.4 s at two). Without `window`, send one and wait for its
  `ACK`. Nothing overflows either way: USB holds the host off until there is room.
- A `D` line that arrives after the `PUT` has ended (it was sent ahead and the
  transfer then failed) is dropped without an answer.
- When `size` bytes have arrived it compares the digest. On a mismatch it removes
  the file and answers `ERR digest`.
- A path in `/os` or `/etc` needs sudo. An admin with a grace (from `AUTH`) may
  write there; without one the answer is `ERR sudo` before `READY`. An ordinary
  account gets `ERR perm` either way.
- The host may send `CANCEL` instead of a `D` line: the partial file is removed
  and the answer is `ERR cancelled`.
- 10 seconds with no line from the host ends the transfer with `ERR timeout`,
  and the partial file is removed.
- An existing file at `path` is replaced.
- A `size` of 0 makes an empty file: `READY` is followed straight away by `OK`,
  with no `D` lines.
- Measured on a Pico 2 W: about 19 KB/s. `GET` runs at about 74 KB/s.

### GET <path>

Reads a file.

```
> GET /os/registry.cfg
< 0x1E SIZE 412
< 0x1E D <base64 of bytes 0..411>
> A
< 0x1E OK sha=5b1c...7e
```

- After each `D` frame the device waits for the host to send a line starting with
  `A` before sending the next, so the host is never flooded.
- The host may send `CANCEL` instead of `A`, which ends with `ERR cancelled`.
- 10 seconds with no line ends it with `ERR timeout`.
- `sha` is the SHA-256 of the bytes sent; the host compares it with what it
  received.
- The account file, the device key and anything in `/os/ssh` need sudo to read:
  an admin without a grace gets `ERR sudo` (protocol 2), as `LS` and `STAT` of
  them do.
- An empty file gives `SIZE 0` and then `OK` straight away, with no `D`.

### LS <path>

```
> LS /home
< 0x1E E d 0 guest
< 0x1E E d 0 root
< 0x1E E f 412 notes.txt
< 0x1E OK n=3
```

One `E` frame per entry: `d` or `f`, the size in bytes (0 for a directory), then
the name, which may contain spaces. `.` and `..` are not listed.

### STAT <path>

```
> STAT /os/registry.cfg
< 0x1E OK type=f size=412
```

`ERR nofile` if it does not exist.

### AUTH <password> (protocol 2)

Proves the signed-in account's password, the way typing it at sudo's prompt
would, and starts the grace for this session: for the next 5 minutes (each use
starts it again) `EXEC sudo ...` runs without asking, and the file requests run
with sudo (`PUT` may write into `/os` and `/etc`, `GET` may read the account
file and the keys). The password is standard base64 of its UTF-8 bytes. The
grace ends with link mode: when the session ends (`BYE`, Ctrl+C or the port
closing), the console is back to asking.

```
> AUTH cm9vdHB3
< 0x1E OK grace=300
```

- `grace` is the number of seconds a pass counts for.
- `ERR auth` for a wrong password, after a pause of a second; every third wrong
  one in a row is logged. `ERR auth` too for an account with no password, and
  `ERR perm` for one that is not an admin: neither can use sudo at all.
- The grace belongs to the console's session, so it also holds for `sudo` typed at
  the prompt after leaving link mode, and `sudo -k` (in an `EXEC` or at the
  prompt) ends it.
- A host asks for the password once (`rpc` takes `$RPC_PASSWORD` if it is set),
  sends `AUTH` when a request first comes back `ERR sudo`, and repeats that
  request. Keeping the password for the rest of the connection lets it send
  `AUTH` again if the grace runs out in the middle of a long job.

### BRIDGE on | off (protocol 3)

The host says it will, or will no longer, answer the device's fetch requests.

```
> BRIDGE on
< 0x1E OK bridge=on chunk=768
> BRIDGE off
< 0x1E OK bridge=off chunk=768
```

- `BRIDGE` alone reports the state without changing it.
- `chunk` is the most bytes the device takes in one `FDATA` line.
- An image with a network of its own (`bridge=0` in `HELLO`) answers `ERR nobridge`.
- The offer outlives `BYE`, so a host can offer it and then hand the port to a
  terminal. It ends with `BRIDGE off`, when the USB host stops asserting DTR (the
  port was closed), when a request the device sent goes unanswered, and at a
  restart. A host that is finishing with the port and will not answer any more
  sends `BRIDGE off` before `BYE`.

### BYE

```
> BYE
< 0x1E OK
< 0x1E BYE
```

The device then returns to the normal prompt.

## The network bridge (protocol 3)

A Pico or Pico 2 has no radio, and an image can be built without Wi-Fi. On those,
`pkg install <name>`, `pkg update`, `pkg upgrade`, `update check`, `update install`,
`wget`, `curl`, `runurl` and a package's `fw_http_*` calls can still reach the web
through the host the board is plugged into. The device asks the host for a URL, the
host fetches it with its own network and its own trusted certificates, and sends the
body back over the console.

The device's own `User-Agent` goes to the server unchanged (the host's is used
when it sends none), so a page answers the bridge as it would answer the board on
Wi-Fi. When the host followed redirects, the answer carries one more header,
`X-RPC-Final-URL: <url>`, saying where it ended up; a browser on the device reads
relative links against that. (The browser tool cannot pass the `User-Agent` on:
browsers forbid setting it.)

It bridges HTTP, not TCP. An image without Wi-Fi has no TCP/IP stack and no TLS, so
the device never sees a socket or a certificate: it sends a method, a URL, headers
and a body, and receives a status, a few headers and the body. Redirects are
followed on the host.

### Offering it

The device sends nothing until a host has said it can answer, so a terminal program
that knows nothing of this never sees a frame.

- In link mode, `BRIDGE on` (above).
- From a terminal (`rpc term`, the browser tool's terminal), the two bytes
  `0x1E 0x1F`, sent at any time. The device answers with one frame,
  `0x1E BRIDGE on chunk=768`. The bytes are control characters: firmware without
  the bridge, the login prompt, the line editor and the full-screen programs all
  ignore them, so sending them is harmless whatever the console is doing. No
  answer within a second or two means no bridge.

The offer belongs to the USB connection, not to a login or a link session, and ends
as `BRIDGE` says.

### Who can use it

Only a command running on the console's own shell: one typed at the prompt (with the
host's terminal attached), or one run by `EXEC`. A background job, a service, a
remote login and a package's worker task cannot, because only the shell's task reads
the console; they get the same "no network" answer as before, with a line saying
why. `sudo` is unchanged: `sudo pkg install gps` still needs it, and in `EXEC` the
grace from `AUTH`.

### One fetch

Device to host, as frames:

```
0x1E FETCH <id> <method> <url>
0x1E FHDR <id> <base64 of one "Name: value" header>       zero or more
0x1E FBODY <id> <base64 of at most 768 bytes>              zero or more
0x1E FGO <id> chunk=768
```

Host to device, as lines that also start with `0x1E`:

```
0x1E FWAIT <id>                                           at once, then every 2 s until FHEAD
0x1E FHEAD <id> <status> [len=<bytes>]
0x1E FHDR <id> <base64 of one "Name: value" header>       zero or more
0x1E FDATA <id> <base64 of at most chunk bytes>           each one waits for FACK
0x1E FDONE <id> ok
0x1E FDONE <id> err <code> <message to show a person>
```

And from the device while the body arrives:

```
0x1E FACK <id> <bytes so far>
0x1E FSTOP <id>
```

- `id` is a decimal number, new for every fetch. A line with any other id is
  dropped.
- `url` is the rest of the line. Only `http://` and `https://` are ever sent.
- The device forwards the request headers it would have sent itself, less `Host`,
  `User-Agent`, `Accept-Encoding`, `Connection` and `Content-Length`. The host
  decides which to use.
- `FGO` ends the request. The host answers `FWAIT` straight away, and every 2
  seconds until it has a response, so the device can tell a slow server from a host
  that is not there.
- `FHEAD` carries the final status, after the host has followed any redirects.
  `len` is given only when the host knows exactly how many bytes it will send (a
  `Content-Length` with no content encoding); without it the body ends at `FDONE`.
  The `FHDR` lines after it are the response headers the host chose to pass on
  (`Content-Type`, `ETag`, `Last-Modified`), each under 200 bytes.
- After each `FDATA` the host waits for the device's `FACK` before sending the
  next, and sends `FDONE ok` only after the last one is acknowledged. The device
  acknowledges a chunk once it has dealt with it (written it to flash, say), so
  the host is held to the speed of the slowest part and the device holds one
  chunk at a time. `FWAIT` may come between chunks while the server is slow.
- `FDONE ok` ends a fetch that went through, whatever its status. `FDONE err`
  ends one that did not. The codes are `refused` (the host will not fetch that URL:
  not http or https, not on its allowlist), `toobig` (over the host's size cap),
  `net` (the host could not reach the server: DNS, a refused connection, a
  certificate that did not verify), `timeout` (the server went quiet), `cancelled`
  (after `FSTOP`) and `badreq`.
- `FSTOP` is the device giving up: Ctrl+C, a body bigger than it can store, a full
  filesystem. The host stops sending and answers `FDONE <id> err cancelled`. The
  device reads and drops lines until that `FDONE`, for at most 3 seconds, so a
  late `FDATA` is never read as a request or typed at the prompt.
- 10 seconds with no line from the host ends the fetch on the device with "the
  computer did not answer", and withdraws the offer.
- A `0x03` from the host during a fetch is Ctrl+C, as it is anywhere else.

The host decides what to fetch. It fetches only `http` and `https`, checks every
redirect the same way, verifies HTTPS with its own trust store, never reads its own
files on the device's behalf, may refuse anything (an allowlist, a size cap; the
host tools refuse the computer's own loopback and link-local addresses unless they
are named), and logs every request it serves.

```
< 0x1E FETCH 3 GET https://raw.githubusercontent.com/.../repo-v2/index.json
< 0x1E FGO 3 chunk=768
> 0x1E FWAIT 3
> 0x1E FHEAD 3 200 len=8121
> 0x1E FHDR 3 Q29udGVudC1UeXBlOiB0ZXh0L3BsYWlu
> 0x1E FDATA 3 ewogICJuYW1l...
< 0x1E FACK 3 768
  ...
> 0x1E FDATA 3 ...
< 0x1E FACK 3 8121
> 0x1E FDONE 3 ok
```

### In a terminal

A host that runs a terminal on the console looks for `0x1E` in what the device
prints, as a link host does, and keeps the bridge frames (`BRIDGE`, `FETCH`, `FHDR`,
`FBODY`, `FGO`, `FACK`, `FSTOP`) off the screen. Everything else, other frames
included, is shown as it comes. While a fetch is running, the host holds the keys
typed at it and sends only Ctrl+C (`0x03`), which stops the fetch.

On the device, a line that starts with `0x1E` arriving where keys are read (the
prompt, the login prompt, a program reading keys, a link request) is never taken as
keys. It is dropped up to the end of its line. Ctrl+C is never dropped.

## The network port

Firmware from v2.0.69x has a second USB serial interface, named "RPCortex
Network", after every other interface (the console stays interfaces 0 and 1). It
carries nothing but the bridge's frames, in the same form as above, and no
console, so a computer can lend the board its connection without the console
being involved, and **any task on the board may use it**: a background job, a
service, a package's work. One fetch at a time; another waits its turn.

- The host opens the port (asserting DTR) and sends `0x1E 0x1F`. The board answers
  `0x1E BRIDGE on chunk=768`. Bytes that are not part of a fetch are ignored.
- The offer ends when the host closes the port (DTR drops) or stops answering in
  the middle of a fetch. A host that keeps the port open offers again from time to
  time while it is quiet, never in the middle of a fetch.
- Which way a request goes: Wi-Fi when the board has it and is joined, then this
  port, then the console's bridge (for a command typed there).
- `rpc bridge` is the host end: it serves every board's network port and refuses
  the computer's own network (private, loopback and link-local addresses) unless
  `--lan`, since the requests come from programs rather than a person.
- Tools looking for the console should leave this port alone: on Linux its
  `interface` is "RPCortex Network".

## Older firmware

Firmware without link mode answers `'link' is not a command`. A host can still:

- send a file with `put <name> <len> <sha256>` and the raw bytes, waiting for one
  `0x06` byte after every 256 bytes (see `tools/putfile.py`);
- run a command by typing it with `&& echo "MK" "<token>0" || echo "MK" "<token>1"`
  appended, and searching the output (anywhere, not only at a line start, since
  the output may not end in a newline) for `MK <token>0` or `MK <token>1`. Quoting
  the halves keeps the echo of the typed line from matching. The prompt takes at
  most 127 characters, and the line editor repaints the line as it is typed.

Both are what the tools did before this protocol existed. Neither returns an exit
status or works for files coming the other way.

## Notes for implementers

- Open the port with DTR asserted. The Pico SDK's USB console counts the host as
  connected only then, and drops output otherwise.
- 115200 is the nominal speed. Over USB the number is ignored, but 1200 is
  special: opening the port at 1200 baud resets the board into BOOTSEL.
- Type nothing at a login prompt that you did not mean to.
- Read what is waiting rather than a fixed count. A reader that asks for 4096
  bytes and waits for its timeout when fewer arrive adds that timeout to every
  chunk's round trip; with pyserial, `read(max(1, port.in_waiting))`.
- Always read to the final `OK` or `ERR`. A host that sends its next request early
  will have it read as the answer to a question (`A`, `D`, `CANCEL`), or as part
  of a running command's input.
