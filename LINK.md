# The link protocol

How a program on a computer drives an RPCortex v2 device over its serial console:
run a command and get its output and exit status back, send a file, fetch a file,
list a directory. It is what `tools/rpc` (the command-line tool) and `tools/web`
(the browser tool) speak.

The console is built for a person: a line editor that echoes and repaints, colour,
prompts, suggestions. A program reading that has to guess where a command's output
starts and ends. Link mode is the same console with all of that switched off and
every answer framed, so nothing has to be guessed.

Protocol version: **1**.

## Entering and leaving

Link mode is a shell command, `link`. It runs as whoever is logged in, with that
account's permissions, exactly as if each request had been typed at the prompt.

A host enters it from a prompt:

1. Send `0x03` (Ctrl+C) and wait about 200 ms. That abandons anything half-typed.
2. Send `link\r`.
3. Read until a `HELLO` frame arrives (below). Everything before it is the echo
   of the command and a one-line notice for a person; ignore it.

If the device answers with a login prompt (`Username`) instead, nobody is logged
in, and the host has to log in first or ask the user to. If it answers
`'link' is not a command`, the firmware is older than link mode; see
"Older firmware" below.

It leaves link mode, and the normal prompt comes back, when:

- the host sends `BYE`;
- a `0x03` byte arrives while it is waiting for a request (so a person who typed
  `link` by accident gets out with Ctrl+C);
- the USB host stops asserting DTR (the port was closed) for 3 seconds.

## Framing

**Host to device:** one request per line, ending `\n` (a `\r` before it is
ignored). ASCII. At most 1100 bytes including the newline. The first word is the
verb; words are separated by one space. Where a request ends in a path or a
command line, that last field is the whole rest of the line, spaces included.

**Device to host:** one frame per line. **A frame starts with the byte `0x1E`**
(ASCII record separator), then the frame type, then optional fields, then `\n`.

```
0x1E TYPE [field ...] \n
```

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

Binary data travels as **standard base64** (RFC 4648 alphabet, `=` padding) in
`D` lines and frames. Each `D` carries at most the `chunk` number of bytes from
`HELLO` (768 in protocol 1, which is 1024 base64 characters).

## Requests

### HELLO

Sent by the device unprompted on entering, and again on request.

```
> HELLO
< 0x1E HELLO proto=1 os=v2.01-beta build=552 board=pico2_w user=root cwd=/ chunk=768 exec_max=32768
< 0x1E OK
```

On entry the `HELLO` frame comes on its own, with no `OK` after it, because there
was no request. `exec_max` is the most output one `EXEC` returns. `cwd` is
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
- A command that stops to ask a question (a yes/no confirmation, a password)
  waits for keys the host is not sending. Use the terminal for those, or send
  `0x03` to give up.

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

### BYE

```
> BYE
< 0x1E OK
```

The device then returns to the normal prompt.

## Older firmware

Firmware without link mode answers `'link' is not a command`. A host can still:

- send a file with `put <name> <len> <sha256>` and the raw bytes, waiting for one
  `0x06` byte after every 256 bytes (see `tools/putfile.py`);
- run a command by typing it with `; echo <marker>` appended and reading until the
  marker appears at the start of a line, keeping in mind that the line editor
  repaints the line it is typing.

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
