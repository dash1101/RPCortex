# Releasing

How a change becomes a release, and how a release reaches the devices, GitHub and the
websites. Everything here is driven by `tools/release.py`; `tools/release.py --help` lists
its commands.

## Versions

`vMAJOR.MINOR.BUILD`, with a label until it is final: `v2.0.640-beta`.

- **BUILD** is the commit count when the images were built. It rises on its own and is never typed
  by hand, so every release has a number no other release has and a later one is always newer.
- **MINOR** moves for a step in what the generation runs on or can do: v2.1 is the ESP32 port.
  It does not move for each release.
- **MAJOR** is a generation. It is the only thing with a codename (v1 Vela, v2 Vela II) and it
  changes when the platform does: a new kernel, a new package ABI. A release never breaks packages;
  a change that would is a new MAJOR.
- **The label** says how finished a release is:

  | Release | Meaning |
  |---|---|
  | `v2.0.640-beta` | A beta of v2.0, built at commit 640. |
  | `v2.0.702-rc`, `v2.0.715-rc2` | Release candidates; each further one counts up. |
  | `v2.0.731` | No longer a beta: the label is dropped. |
  | `v2.1.800-beta` | The next minor, a beta again until it settles. |

- Devices compare `MAJOR.MINOR.BUILD` (`2.0.640`), so a later release always beats an earlier one.
  The label is not part of the comparison.

**Earlier releases.** v2.00-beta (tag `v2.0.0`) through v2.05-beta used `vMAJOR.ITER`, an iteration
count. They keep their tags and their notes. A device running one of them compares `2.05.635` with
`2.0.640` and sees the new form as older, so it moves across once with `sudo update online --force`
(or a reflash); after that, updates are offered as usual.

**Builds are not releases.** Only what is cut with `tools/release.py` is published; `ver` shows the
build of whatever is running, so the builds in between are told apart by their number.

The version in `os/kernel/kernel.h` is the line and label the **next** release will carry (MAJOR,
MINOR, label); the build is added when it is cut. `tools/release.py label beta|rc|final` changes the
label (counting release candidates automatically), and `bump minor` / `bump major "Name"` move the line.

## When a release is cut

- **Every push of `main` to GitHub is a release.** Commit locally as the work happens, and when it
  is time to publish, `tools/release.py cut --publish` builds, tags, pushes and creates the GitHub
  release in one go. Nothing reaches `main` on GitHub without a version number of its own.
- **An urgent fix goes out on its own**, straight away, as its own release. It is not held back
  for other work that is not ready.
- The build number makes each release's version its own. The label says how finished the line is:
  `-beta`, then `-rc` and `-rc2` and so on, then no label (see Versions above).
- The `Summary:` line names the most important change first. A release with only Fixed and
  Security entries is a fix release and its summary says so ("Fixes for ..."). One with New
  entries leads with the biggest new thing.

## Writing the notes as the work happens

`CHANGELOG.md` starts with an **Unreleased** section. Anything a person using the device, a
package author, or someone on the website would notice goes in it when the change lands, not at
release time. Every entry goes under exactly one of six categories, always in this order:

```
## Unreleased

Summary: One plain sentence on what this release is.

### New
### Improved
### Fixed
### Security
### Changed
### Known issues
```

| Category | What goes in it | Example |
|---|---|---|
| **New** | Something that did not exist before: a command, a feature, a package API call, a supported board, a tool. | `link`: a mode for programs on a computer to run commands and move files. |
| **Improved** | Something that already existed and now works better: faster, smaller, less memory, clearer messages, easier to use. Say by how much. | Packages are 7 to 19% smaller on disk (Nova D1 440 KB to 365 KB). |
| **Fixed** | Something that was wrong. Say what was wrong and what is true now. | `wc -l file` said "No such file: -l". It counts lines now. |
| **Security** | Anything about who can do what, or what is exposed: permission checks, accounts, passwords, network services, certificates. A security fix goes here, not under Fixed. | `mkdir` now asks the same permission question as every other command that writes. |
| **Changed** | Something that now behaves differently in a way a person may need to act on: a renamed or removed command, a different default, a package API change, a new file layout. Anything that breaks existing use goes here, and says what to do. | `download` only runs at the prompt; in the background it never saw a key. |
| **Known issues** | Open problems a reader would otherwise trip over, and anything that has not run on real hardware yet. | The Pico and Pico W images have not been booted on a board. |

Guidelines for an entry:

- One line, or two when it needs them. Say what a person sees, not how it was built.
- Name the command or the screen: "`update check` no longer offers a build the device rolled back
  from", not "fixed the refused marker".
- Use numbers when there are some (sizes, times, before and after).
- Write it plainly, the way it would be said out loud. No dashes used as punctuation; use commas,
  periods or parentheses.
- The `Summary:` line becomes the headline of the release, so write it for someone who reads only
  that line. A placeholder or an empty summary stops the release, and so does a heading that is
  not one of the six categories (its entries would otherwise be left out).
- The older names `Added` and `Known limitations` are still read, as New and Known issues.

## Cutting a release

1. Be on `main` with everything committed, and Unreleased written.
2. `tools/release.py status` shows the version, the build the images would carry, the notes and the
   board status.
3. `tools/release.py cut` is a dry run. It prints every step and the release notes exactly as GitHub
   will show them, and changes nothing.
   Before publishing, flash the new images onto every board on hand and run `tools/pretest.py`
   (`--out` keeps a report per board). It installs and runs the `selftest` package on each board
   (memory, filesystem, timers, tasks, the shell, the registry, and the network: DNS, HTTPS, and a
   certificate that must be refused) and times a 128 KB transfer each way. Any FAIL stops the
   release; compare the readings with the last report, since a check can pass and still be slower.
   Do not edit the source while `release.py` builds: it builds the working tree.
4. `tools/release.py cut --publish` then:
   - builds all four boards (image checks and the host suites run as part of each),
   - checks each image was built as this version and build,
   - writes `releases/latest.json` and the images it points at, which is what `update install` reads,
   - signs `latest.json` and writes `releases/latest.json.sig` beside it (Signing, below),
   - writes versioned copies and `SHA256SUMS` for the GitHub release,
   - turns Unreleased into a dated section of `CHANGELOG.md` and adds a fresh one,
   - adds the release to `releases/releases.json`, which the websites read,
   - commits, tags (the version as written: `v2.01-beta`, `v2.06`), pushes, and creates the GitHub release,
   - then steps `kernel.h` to the next number and commits that (an rc becomes the next rc).

A tag is never moved or deleted by the tool. A mistake in a release is fixed by the next one.

## Signing

Firmware checks that `latest.json` is signed by an RPCortex release key before `update check` or
`update install` uses it, over the board's own Wi-Fi or through the bridge. The list names each
image's SHA-256, so a signed list covers the images as well.

- **Two keys**, Ed25519. The primary signs every release. The backup stays offline and signs only if
  the primary is lost; firmware accepts either, so losing one key never strands a board. The public
  halves are in `os/core/relsig.cpp`. Replacing a key means a release, signed by a key the boards
  already trust, whose firmware lists the new one.
- **The private keys are never in a repository.** `release.py` reads `$RPC_SIGNING_KEY`, or
  `~/.config/rpcortex/release-signing/primary.pem`, and signs with the `openssl` command. A dry run
  signs a scratch copy, so a missing key shows up before `--publish`. Without a key nothing is
  published, because every board that checks would refuse the list.
- **`latest.json.sig`** is one line per signature, `ed25519 <key id> <base64>`, the key id being the
  first 8 hex digits of SHA-256 of the public key. Committed with `latest.json`, in the same commit.
- **The override** is `sudo update install --unsigned`: for someone running their own fork or a mirror.
  `update from-file`, a `.uf2` and BOOTSEL never read the list and are unchanged.
- **A test image** that reads its list from elsewhere: `RPC_TEST_MANIFEST_URL=<url> ./build.sh <board>`.
  It is named `-testlist` and `release.py` refuses it.

## Where a release shows up

| Place | How it updates |
|---|---|
| A device | `update check` reads `releases/latest.json` from `main`. It is live the moment the release commit is pushed. |
| The GitHub release | Created by `cut --publish`: the summary, the notes by section, an install table with each board's hardware status, the images and `SHA256SUMS`. |
| The websites: release list, Get page, version on the home page | Read `releases/releases.json` and `releases/boards.json` from `main` in the browser. Nothing is edited on either site for an ordinary release. |
| `CHANGELOG.md` | Rolled by the tool. |

## What is still done by hand

- **`releases/boards.json`**, when a board is tested: set `status` to `verified` and
  `verified_build` to the build that was run. The Get page and the release notes show it.
  A board that is not listed as verified is shown as not yet booted.
- **A new major** also needs its docs set (one per major, with "since v2.4" notes where behaviour
  differs) and its codename page on the site. `bump major "Name"` prints a reminder.
- **Package releases** are separate: `package.cfg`, the rebuilt `.pkg` and `repo/index.json`, as
  described in the package repository.

## Rules

- `releases/releases.json` and `releases/latest.json` are generated. Fix the notes in
  `CHANGELOG.md` and the code, not the generated files.
- Each release gets its own tag, named by its version. There is no moving "latest beta" tag.
- A release is cut from a commit that passes CI. The tool refuses a dirty tree.
- The first release, `v2.00-beta`, was published before this tool existed, under the tag `v2.0.0`. It is
  in `releases/releases.json` as `v2.00-beta`, and the tag stays as it is.
