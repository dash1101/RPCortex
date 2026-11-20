# The link protocol

How a program on a computer drives an RPCortex v2 device over its serial console:
run a command and get its output and exit status back, send a file, fetch a file,
list a directory. It is what `tools/rpc` (the command-line tool) and `tools/web`
(the browser tool) speak.

The console is built for a person: a line editor that echoes and repaints, colour,
prompts, suggestions. A program reading that has to guess where a command's output
starts and ends. Link mode is the same console with all of that switched off and
every answer framed, so nothing has to be guessed.

Protocol version: **2**. Version 2 added `AUTH` and the `sudo` error, for firmware
where installing a package or changing `/os` or `/etc` needs sudo (PERMISSIONS.md).
A host reads `proto` from `HELLO` and does not send `AUTH` to a device that says 1.

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

Binary data travels as **standard base64** (RFC 4648 alphabet, `=` padding) in
`D` lines and frames. Each `D` carries at most the `chunk` number of bytes from
`HELLO` (768 in protocol 1, which is 1024 base64 characters).

## Requests

### HELLO

Sent by the device unprompted on entering, and again on request.

```
> HELLO
< 0x1E HELLO proto=2 os=v2.03-beta build=600 board=pico2_w arch=armv8m user=root cwd=/ chunk=768 exec_max=32768
< 0x1E OK
```

On entry the `HELLO` frame comes on its own, with no `OK` after it, because there
was no request. `exec_max` is the most output one `EXEC` returns. `arch` is the package
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
  byte, which interrupts it the way Ctrl+C does at the prompt.
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
< 0x1E READY chunk=768
> D <base64 of bytes 0..767>
< 0x1E ACK 768
> D <base64 of bytes 768..1435>
< 0x1E ACK 1436
< 0x1E OK sha=9f0e...c2
```

- The device checks the path, the permission and the free space before `READY`,
  and answers `ERR` instead if any of them fails.
- After each `D` line it writes the bytes and answers `ACK <bytes so far>`. The
  host waits for that `ACK` before sending the next `D`. Flash writes are slow
  and stall the other core, so this is what keeps the console from overflowing.
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

### BYE

```
> BYE
< 0x1E OK
< 0x1E BYE
```

The device then returns to the normal prompt.

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
