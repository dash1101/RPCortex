# SSH

The Pico 2 W can be logged in to over the network with any SSH client, and can
log in to other machines itself. Both are built into the firmware for that board
only; the Pico W has no room left for them and the boards without a radio have
nothing to carry them over.

**Not yet run on a board.** The protocol code is tested on a computer against the
real OpenSSH client and server (see Testing, below), and the firmware builds. The
part that only a device has — the sockets, the tasks, the accounts and files — has
not been tried on a Pico 2 W yet.

## Logging in to the device

```
ssh server on
```

The first time, this makes the device's host key and shows its fingerprint:

```
[@] [ssh] Server on, port 22. It starts again at every boot.
  host key  ECDSA SHA256:aAaO7xXPv7rSw893E3aHYjlB4JBaiHw06vfp15NIRvU
```

Then, from a computer on the same network:

```
ssh root@192.168.1.40
```

The client shows a fingerprint the first time it connects. It should match the one
above (`ssh server fingerprint` prints it again). Logged in, it is the device's
shell: the same commands, pipes, `&&`, Tab completion, colours, and Ctrl+C stops the
running command. `logout` or `exit` ends the session. One command at a time works
too, with its exit status passed back:

```
ssh root@192.168.1.40 'ls /home'
```

`ssh server status` shows who is logged in and from where; `ssh server off` stops
it, ending any sessions. `ssh server port 2222` moves it (from the next start).

### Keys instead of passwords

Put the public key from the computer (`~/.ssh/id_ecdsa.pub` or `~/.ssh/id_rsa.pub`)
on its own line in `/home/<user>/.ssh/authorized_keys` on the device. A key line is
longer than the console's 128-character line, so it is easier to copy the file
than to type it: put it on the `download` drive, then

```
mkdir /home/root/.ssh
cat /usb/id_ecdsa.pub >> /home/root/.ssh/authorized_keys
```

ECDSA (P-256) and RSA keys of 2048 bits or more work. Ed25519 keys do not: the
crypto library has no Ed25519.

A line with options in front of the key (`from=`, `command=`, `no-pty`...) is
ignored, not honoured without its options: a key that was meant to run one
command must not get a shell.

## Logging in from the device

```
ssh dash@192.168.1.10
ssh -p 2222 dash@laptop.local 'uptime'
```

The first connection to a host shows its key's fingerprint and asks for `yes`
before going on; the key is then kept in `~/.ssh/known_hosts`, and a different key
later stops the connection before a password is sent. Passwords are asked for with
the same prompt the device uses everywhere. `Ctrl+]` ends a connection from this
side; every other key, Ctrl+C included, goes to the far end.

`ssh keygen` makes a key for the signed-in user and prints the line to add to
`authorized_keys` on other machines; after that `ssh` uses it before asking for a
password. `ssh pubkey` prints the line again.

## What is supported

| | |
|---|---|
| Key exchange | curve25519-sha256 (and @libssh.org), ecdh-sha2-nistp256 |
| Host key | ecdsa-sha2-nistp256 (the client also accepts RSA host keys, rsa-sha2-256/512) |
| Ciphers | aes128-gcm@openssh.com, aes256-gcm@openssh.com, aes128-ctr, aes256-ctr |
| MACs (with CTR) | hmac-sha2-256-etm@openssh.com, hmac-sha2-256 |
| Login | publickey (ECDSA P-256, RSA 2048-8192 with SHA-2), password; the client also does keyboard-interactive |
| Session | a shell with a terminal, or one command; window size changes; Ctrl+C |
| Other | strict key exchange (the Terrapin fix), re-keying, no compression |

Not supported: port forwarding of any kind, agent and X11 forwarding, `scp` and
`sftp`, environment variables, Ed25519, ChaCha20-Poly1305.

## Security

- **Off until turned on**, and only an admin can turn it on. In a maintenance boot
  it stays off whatever the setting says.
- **Accounts without a password cannot log in**, by password or by key. `guest` is
  password-less as shipped; `passwd guest` gives it one if it should be reachable.
- **A remote session is its own user.** A guest logged in over SSH is a guest even
  while root is signed in at the console, and the other way round. Everything a
  session starts (`bg` jobs, packages) keeps that user.
- **Login limits.** Six attempts per connection, a pause after each wrong password,
  a minute to finish logging in, and after ten refusals in a minute from anywhere,
  nobody gets in for a minute. Two sessions at most. Every login and every refusal
  is in `logdump`.
- **Files that decide who gets in are protected.** The host key lives in `/os/ssh`,
  which only an admin can read. An ordinary account cannot read or change anyone
  else's `~/.ssh`, so it cannot add itself to root's `authorized_keys`.
- **It is not audited.** It is built on mbedTLS's primitives and tested against
  OpenSSH, but nobody independent has reviewed it. Keep it to networks you trust.

## Limits worth knowing

- Output printed while a command holds the filesystem or the network (an `ls` of a
  huge directory, `curl` to the screen) can only be held back 8 KB at a time;
  past that it is dropped and the session says `[ssh: output lost]`. `curl -o file`
  is unaffected.
- A session uses about 25 KB of memory, its 8 KB stack included, and the
  server's own task another 8 KB. A login's key exchange runs on the console's
  core and can pause the console for a moment — and anyone who can reach the port
  can keep that core busy by connecting over and over, before logging in.
- Remote sessions share the device's aliases and settings with the console;
  per-user `User.` settings are the console user's. A package run from a remote
  session by an account that is not an admin may only write its own settings
  (`Apps.*`, `User.*`).

## Testing

On a computer, from `os/host`:

```
./run_all.sh            # everything; the SSH parts are below
```

- `ssh_test` checks the encodings, the key exchange maths, signatures and
  fingerprints against published values and against OpenSSH's own output, and
  plays a client against the server with a deliberately misbehaving peer.
- `ssh_fuzz_test` feeds damaged input to everything a peer can send.
- `ssh_interop_test.sh` runs the server code behind a local socket and connects the
  system's `ssh` to it with every algorithm in turn, then runs the client code
  against that server and against a real `sshd`. It says SKIPPED when there is no
  `ssh` to test against.
- `identity_test` proves the per-session identity: a guest session cannot read
  protected files or run admin commands while root is at the console.
