# Permissions and sudo

Three kinds of command. Anything a signed-in account may run, things that need an
admin, and things that change the system: packages, accounts, firmware, what starts
at boot, the network login, and the files in `/os` and `/etc`. The third kind runs
only as `sudo <command>`, by an admin, after that admin's own password.

```
root@vela:/> pkg install gps
[!] 'pkg install' changes the system. Run it with sudo: sudo pkg install gps
root@vela:/> sudo pkg install gps
Password for root, to run 'pkg install' ••>  ••••••
[@] [pkg] Installed gps 1.2
```

Why it is there: being signed in as an admin is not proof that anybody knows the
password. Autonomy signs root in at boot with no prompt, and a session left open
at the serial port is the same. So the commands that would let somebody keep the
device ask every time, and the everyday ones (reading, Wi-Fi, a restart) do not.

## sudo

- `sudo <command>` asks for the signed-in account's password (not root's), three
  tries, on the terminal the command came from, then runs that one command with
  sudo. A wrong password is followed by a pause, and three of them are logged.
- A password counts for **5 minutes** in that session, and each use starts the time
  again, so a run of commands asks once. The console is one session and each SSH
  login is another: a password typed in one does nothing for the others. Somebody
  else signing in at the console starts without it.
- `sudo -v` asks now. `sudo -k` forgets it. `sudo -l` lists what needs sudo.
- Only an admin can use sudo. An ordinary account typing it is refused without being
  asked anything, and the attempt is logged. An account with no password (`guest`)
  cannot authorise anything.
- A redirect after a sudo'd command is written with that sudo:
  `sudo echo 1 > /etc/flag` works. (A shell on a computer would not do that.)
- There is no `sudo -u` and no sudoers file: every admin may use sudo, and it always
  acts as that admin.

## What needs what

| Command | Anyone | Admin | Admin with sudo |
|---|---|---|---|
| `pkg` | | `list`, `search`, `info`, `update` (the list only), `repo`, `certs` | `install`, `remove`, `upgrade`, `repo add`/`remove`, `certs install` |
| `stock` | `list` | | installing a built-in package |
| `run` | a script; a package on a Pico 2 or Pico 2 W | | a package on a Pico or Pico W, where packages run with the OS's own privileges |
| `update` | | `check`, `rollback` (what is saved) | `install`, `from-file`, `rollback --now`, `--save`, `--forget` |
| `ssh` | the client, `keygen`, `pubkey`, `server status`, `server fingerprint` | | `server on`, `off`, `port`, `rekey` |
| `autonomy` | | `status` | `on`, `off` |
| `startup` | | `list` | `add`, `remove`, `clear`, `run` |
| `task` | | `list` | `add`, `remove`, `clear`, `start`, `stop`, `reload` |
| `service` | | `list`, `status` | `add`, `remove`, `clear`, `start` |
| `mkacct` | | an ordinary account (asks the password too) | `--admin` |
| `passwd` | your own (an account with no password cannot give itself one) | | somebody else's: `sudo passwd <name>` |
| `usermod`, `rmuser` | | | every form |
| `regreset`, `factoryreset`, `failboot` | | | every form |
| `safeboot` | | a plain restart into maintenance | with a command to run there |
| `reg` | | reading; setting anything else | `set` of `System.Autonomous`, `System.Setup`, `System.Active_User`, `System.SafeBoot*`, `SSH.*`, `Alias.*`, `Pkg.Repos` |
| Files in `/os` and `/etc` | reading (but not the account file, the registry, the device key or `/os/ssh`) | reading, but not the account file, the device key or `/os/ssh` | changing anything: `rm`, `mv`, `cp`, `touch`, `mkdir`, `rename`, `edit`, a redirect, `put`, link's `PUT`, a package's file calls; reading the account file, the device key and the SSH host key |

Everything else is as it was. `reboot`, `wifi`, `radio`, `date`, `pulse`, `bt`,
`settings`, `unload`, `pkgdisable`, `pkgenable`, `fscheck`, `alias` and the rest of
the admin commands need an admin and no sudo; `ls`, `cat`, `ps` and the rest need
nobody in particular.

A few that might look as if they belong in the last column, and why they do not:

- `pkg update` only fetches the list of what is available. Installing from it needs sudo.
- `fscheck --fix` puts back missing directories and rewrites the backup copies of the
  settings and accounts. It removes nothing anybody made.
- `pkgdisable` and `pkgenable` only decide whether a package that is already
  installed loads at boot. They are what to reach for when a package stops the device
  starting, so they stay one step away.
- `bootloader` restarts into USB flashing mode. Any computer on the USB cable can do
  that anyway (by opening the port at 1200 baud), so a password on it would protect
  nothing.

`sudo -l` prints the same list from the commands themselves.

### System files

`/os` and `/etc` hold everything the device runs as the console's account at the next
start: startup items, services and scheduled tasks, installed packages and their
index, the trusted certificates, a staged update. An ordinary account can never
change them. An admin can, with sudo.

Reading is as it was, with one exception: the account file (its salted password
hashes can be guessed offline), the device key that seals the saved Wi-Fi
passwords, and the SSH host key need sudo to read, for an admin too. Otherwise
being signed in, by autonomy or over SSH with a key, would be a way to the very
password sudo asks for.

That rule is for a person's commands, a package's file calls and the host tools'
`PUT`. The OS's own writes are not commands and are not asked: saving the settings,
the account file, `sudo pkg install` writing `/os/pkg`, an update staging its image.

## Work nobody is watching

Startup items, services, scheduled tasks and a command staged for a maintenance boot
run **with sudo already**. They come from `/etc` and from settings only sudo can
write, so whoever put them there proved the password doing it. Whatever they start
runs with it too.

Nothing is ever asked for where nobody can answer. A `bg` job, a package's worker, a
line a package runs (`fw_shell_run`) and the host tools' `EXEC` never prompt. They
use the session's 5 minutes if it has them (a `bg` job belongs to the session that
started it), and otherwise are refused with one line:

```
[!] sudo needs a password and this job has no terminal; run `sudo -v` first.
```

A package run with sudo (`sudo novad1 setup`) runs its own lines with it. One run
without sudo cannot ask for the password, so it either uses the session's grace or
is refused. Nova D1's screen normally runs as a service, so its installs, resets and
service edits work as before; started by hand, they need `sudo -v` at the console
first.

## Over SSH

A remote login is its own session, with its own 5 minutes. In a session with a
terminal, sudo asks on that terminal as at the console. A command run without one,
`ssh dev 'sudo pkg install gps'`, is refused, because the password would be typed
into something that echoes it or stores it in a script:

```
[!] sudo needs a terminal to ask for the password; connect with ssh -t.
```

`ssh -t dev 'sudo pkg install gps'` asks for it. A login never inherits sudo from
whatever started its task.

## The host tools

`rpc` and the browser tool send `AUTH` (link protocol 2, see LINK.md) the first time
the device says a request needs sudo, with the password asked for once (or taken
from `$RPC_PASSWORD` for `rpc`), and then repeat the request. `rpc --sudo` asks
first, for a long job that should not stop half way. With older firmware they ask
nothing, because there is nothing to ask.

## Forgotten passwords

Physical presence is enough, and it is the only way.

- **In a maintenance boot, sudo at the console does not ask.** `safeboot` restarts
  into one (as does the recovery ladder after failed starts). At the console's own
  prompt there, `sudo usermod root passwd` sets a new password with no old one.
  It says so on the screen and in the log. Nothing a remote login can do
  reaches it: the SSH server stays off in a maintenance boot, a remote session is
  asked as usual even if the server is started there, and background jobs and the
  host tools' `EXEC` are not let through either.
- **If no admin can sign in at all** (autonomy off, and the only admin's password
  forgotten), the accounts can only be cleared by erasing the filesystem, and every
  file goes with them. Hold BOOTSEL, plug the board in, and erase from the start of
  the filesystem to the end of the flash with `picotool erase -r <from> <to>`. The
  filesystem starts after the firmware reserve and the package slots, counted from
  0x10000000; a build writes both sizes (`fw_reserve.txt`, `pkg_region.txt` in its
  build directory). The next
  start runs first-run setup. Reflashing the firmware on its own keeps the files, and
  the accounts with them.

This is deliberate. Anybody who can hold the board can reflash it with firmware of
their own, so a password the console could not get round would protect nothing and
could lose the owner their device.

## Testing

`os/host/perms_test` holds every rule above to what it says, and checks that the
shell's own sources register each command with the level the tests are about.
`os/host/sudo_test` runs the gate, the password, the grace, the background job, the
service, SSH with and without a terminal, link's `AUTH` and the maintenance boot on
real task switches. `ssh_interop_test.sh` does the SSH part against a real OpenSSH
client. What only a board can show (the prompt on a real terminal, the recovery
boot) is in TESTING.md.
