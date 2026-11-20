# SSH

The Pico 2 W can be logged in to over the network with any SSH client, and can
log in to other machines itself. Both are built into the firmware for that board
only; the Pico W has no room left for them and the boards without a radio have
nothing to carry them over.

The protocol code is tested on a computer against the real OpenSSH client and
server (see Testing, below), and the server has been used from a Linux desktop to
a Pico 2 W. What has and has not been tried on the board is listed at the end.

## Logging in to the device

```
sudo ssh server on
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

`ssh server status` shows who is logged in and from where; `sudo ssh server off`
stops it, ending any sessions. `sudo ssh server port 2222` moves it (from the next
start). Turning the server on or off, moving it and replacing its key all need
sudo (PERMISSIONS.md).

### sudo in a remote session

A remote login is its own session for sudo, with its own 5 minutes: a password
typed at the console does nothing for it, and the other way round. With a terminal
(an ordinary `ssh root@device`, or `ssh -t ... 'command'`), sudo asks on that
terminal. A command run without one is refused instead of asking, because the
password would be typed into something that echoes it:

```
$ ssh root@192.168.1.40 'sudo pkg install gps'
[!] sudo needs a terminal to ask for the password; connect with ssh -t.
$ ssh -t root@192.168.1.40 'sudo pkg install gps'
Password for root, to run 'pkg install' ••>
```

A login logged in by key still needs its account's password for sudo.

### Keys instead of passwords

Put the public key from the computer (`~/.ssh/id_ed25519.pub`, `~/.ssh/id_ecdsa.pub`
or `~/.ssh/id_rsa.pub`) on its own line in `/home/<user>/.ssh/authorized_keys` on
the device. A key line is longer than the console's 128-character line, so it is
easier to copy the file than to type it: put it on the `download` drive, then

```
mkdir /home/root/.ssh
cat /usb/id_ed25519.pub >> /home/root/.ssh/authorized_keys
```

Ed25519 keys (what `ssh-keygen` makes unless told otherwise), ECDSA (P-256) keys
and RSA keys of 2048 bits or more all work. An Ed25519 login is the slowest of the
three: checking its signature takes about half a second, against about a third
of that for ECDSA. On a Pico 2 W a whole login took 2.0 to 2.4 s with an Ed25519 key
and 1.8 s with ECDSA.

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

A server's key may be ECDSA, RSA or Ed25519. Ed25519 is asked for last, so a host
already in `known_hosts` under its ECDSA or RSA key goes on presenting that one,
and a host that has only an Ed25519 key is still reachable.

`ssh keygen` makes a key for the signed-in user and prints the line to add to
`authorized_keys` on other machines; after that `ssh` uses it before asking for a
password. `ssh pubkey` prints the line again.

## What is supported

| | |
|---|---|
| Key exchange | curve25519-sha256 (and @libssh.org), ecdh-sha2-nistp256 |
| Host key | ecdsa-sha2-nistp256 (the client also accepts RSA host keys, rsa-sha2-256/512, and ssh-ed25519) |
| Ciphers | aes128-gcm@openssh.com, aes256-gcm@openssh.com, aes128-ctr, aes256-ctr |
| MACs (with CTR) | hmac-sha2-256-etm@openssh.com, hmac-sha2-256 |
| Login | publickey (Ed25519, ECDSA P-256, RSA 2048-8192 with SHA-2), password; the client also does keyboard-interactive |
| Session | a shell with a terminal, or one command; window size changes; Ctrl+C |
| Other | strict key exchange (the Terrapin fix), re-keying, no compression |

Not supported: port forwarding of any kind, agent and X11 forwarding, `scp` and
`sftp`, environment variables, ChaCha20-Poly1305, and Ed25519 for the device's own
keys (its host key and the key `ssh keygen` makes are ECDSA).

## Security

- **Off until turned on**, and only an admin with sudo can turn it on. In a
  maintenance boot it stays off whatever the setting says.
- **sudo is per session.** What changes the system (packages, accounts, firmware,
  what starts at boot, `/os` and `/etc`) needs the account's own password typed in
  that session, so a login made with a key cannot do those things on the strength
  of the key alone. A login never inherits sudo from whatever started its task, and
  the recovery that lets the console use sudo without a password (a maintenance
  boot) never applies to a remote session.
- **Accounts without a password cannot log in**, by password or by key. `guest` is
  password-less as shipped; `passwd guest` gives it one if it should be reachable.
- **A remote session is its own user.** A guest logged in over SSH is a guest even
  while root is signed in at the console, and the other way round. Everything a
  session starts (`bg` jobs, packages) keeps that user.
- **Login limits.** Six attempts per connection, a pause after each wrong password,
  a minute to finish logging in, and after ten refusals in a minute from anywhere,
  nobody gets in for a minute. Two sessions at most. Every login and every refusal
  is in `logdump`.
- **Files that decide who gets in, or what runs as root, are protected.** The host
  key lives in `/os/ssh`, which only an admin can read. An ordinary account cannot
  change anything in `/os` or `/etc` (startup items, services, installed packages,
  trusted certificates), or in anyone else's home, and cannot read anyone else's
  `~/.ssh`, so it cannot add itself to root's `authorized_keys`. An admin needs
  sudo to change `/os` and `/etc`. A package it runs over SSH may only write its
  own settings (`Apps.*`), and `kill` stops only its own tasks.
- **It is not audited.** It is built on mbedTLS's primitives, with TweetNaCl's
  public-domain arithmetic for checking Ed25519 signatures (mbedTLS has none), and
  tested against OpenSSH, but nobody independent has reviewed it. Keep it to
  networks you trust.

## Limits worth knowing

- Output printed while a command holds the filesystem or the network (an `ls` of a
  huge directory, `curl` to the screen) can only be held back 8 KB at a time;
  past that it is dropped and the session says `[ssh: output lost]`. `curl -o file`
  is unaffected.
- A session uses about 25 KB of memory, its 8 KB stack included, and the
  server's own task another 8 KB. A login's key exchange runs on the console's
  core and can pause the console for a moment, and anyone who can reach the port
  can keep that core busy by connecting over and over, before logging in. An
  Ed25519 key adds about half a second to that pause, and only for a key that is
  in `authorized_keys`: an unknown key is refused before its signature is checked.
- Remote sessions share the device's aliases and settings with the console;
  per-user `User.` settings are the console user's, so a remote session reads
  those and (as an admin) writes them. `Apps.*`, which an ordinary account's
  packages may write, includes package settings such as Nova D1's pin map.
- There is no command history (up-arrow) in a remote session yet, and re-keying
  happens by data volume, not by time.

## Testing

On a computer, from `os/host`:

```
./run_all.sh            # everything; the SSH parts are below
```

- `ssh_test` checks the encodings, the key exchange maths, signatures and
  fingerprints against published values and against OpenSSH's own output, and
  plays a client against the server with a deliberately misbehaving peer.
- `ed25519_test` holds the Ed25519 check to RFC 8032: the RFC's own test vectors,
  taken from its text, and the cases its strict rules are there for (S out of
  range, keys and R values that do not decode, a signature that only a cofactored
  check would accept).
- `ssh_fuzz_test` feeds damaged input to everything a peer can send.
- `ssh_interop_test.sh` runs the server code behind a local socket and connects the
  system's `ssh` to it with every algorithm in turn, then runs the client code
  against that server and against a real `sshd`. It says SKIPPED when there is no
  `ssh` to test against.
- `identity_test` proves the per-session identity: a guest session cannot read
  protected files or run admin commands while root is at the console.
- `sudo_test` proves sudo per session: a remote login with a terminal is asked on
  it, one without a terminal is refused, a job from a login uses that login's
  grace and nobody else's, and a login started by a task running with sudo does
  not have it. `ssh_interop_test.sh` checks the same with a real OpenSSH client,
  with and without `-tt`.

On a Pico 2 W (OpenSSH 10.0 on Linux as the other end), the server has done:
logging in with an ECDSA key and with a password, a wrong password refused, a
command's exit status, an interactive session with Tab and Ctrl+C, two sessions
at once, 20 logins in a row with no memory lost (about 1.3 s each, key exchange
included), and 64 KB of binary output byte for byte. Not yet on hardware: the
device's own `ssh` client, full-screen apps over SSH, and the failure cooldowns.
Logging in with an Ed25519 key works too, and an unknown one is refused.
