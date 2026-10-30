# Releasing

How a change becomes a release, and how a release reaches the devices, GitHub and the
websites. Everything here is driven by `tools/release.py`; `tools/release.py --help` lists
its commands.

## Versions

`vMAJOR.ITER`, with the build number beside it: `v2.01-beta (build 520)`.

- **MAJOR** is a generation. It is the only thing with a codename (v1 Vela, v2 Vela II) and it
  changes when the platform does: a new kernel, a new package ABI, a new architecture.
- **ITER** is the number of the release, written with two digits, with no ceiling. **Every release
  published to GitHub takes the next number, whatever it is labelled.** A number is never reused and
  a later number is always newer.
- **The label** says how finished a release is:

  | Release | Meaning |
  |---|---|
  | `v2.00-beta` | The first release of v2: the beta published on GitHub on 2026-10-28 (tag `v2.0.0`). |
  | `v2.01-beta` | The next one published, still a beta. |
  | `v2.03-rc`, `v2.04-rc2`, `v2.05-rc3` | Release candidates; each further one counts up. |
  | `v2.06` | No longer a beta: the label is dropped, and the number keeps counting. |

- **The build number** is the commit count when the images were built. It rises on its own and is
  never typed by hand. Devices compare `MAJOR.ITER.BUILD` (`2.01.520`), so a later release, and a
  later build of the same release, always beats an earlier one. The label is not part of the comparison.
- A release never breaks packages. A change that would is a new MAJOR.

**Builds are not releases.** A release is cut on a schedule, when there is enough to say, and the
builds in between are not published; `ver` tells them apart by build number. That keeps the schedule
steady and the work per release small: the unit of work is the changelog entry, written as the change
lands, not a release per commit.

The version in `os/kernel/kernel.h` is the one the **next** release will carry. `tools/release.py label
beta|rc|final` changes the label (counting release candidates automatically), `bump iter` and `bump major
"Name"` change the number, and cutting a release steps `kernel.h` to the following number by itself.

## Writing the notes as the work happens

`CHANGELOG.md` starts with an **Unreleased** section. Anything a person using the device or the
website would notice goes in it when the change lands, not at release time:

```
## Unreleased

Summary: One plain sentence on what this release is.

### Added
- A new thing, in words a user would use.

### Changed
- Something that now behaves differently.

### Fixed
- What was wrong and what happens now.

### Known limitations
- Anything open that a reader would otherwise trip over.
```

Guidelines for an entry:

- One line, or two when it needs them. Say what a person sees, not how it was built.
- Name the command or the screen: "`update check` no longer offers a build the device rolled back
  from", not "fixed the refused marker".
- A fix says what was wrong and what is true now.
- Anything that has not run on hardware says so, under Known limitations.
- The `Summary:` line becomes the headline of the release, so write it for someone who reads only
  that line. A placeholder or an empty summary stops the release.

## Cutting a release

1. Be on `main` with everything committed, and Unreleased written.
2. `tools/release.py status` shows the version, the build the images would carry, the notes and the
   board status.
3. `tools/release.py cut` is a dry run. It prints every step and the release notes exactly as GitHub
   will show them, and changes nothing.
4. `tools/release.py cut --publish` then:
   - builds all four boards (image checks and the host suites run as part of each),
   - checks each image was built as this version and build,
   - writes `releases/latest.json` and the images it points at, which is what `update install` reads,
   - writes versioned copies and `SHA256SUMS` for the GitHub release,
   - turns Unreleased into a dated section of `CHANGELOG.md` and adds a fresh one,
   - adds the release to `releases/releases.json`, which the websites read,
   - commits, tags (the version as written: `v2.01-beta`, `v2.06`), pushes, and creates the GitHub release,
   - then steps `kernel.h` to the next number and commits that (an rc becomes the next rc).

A tag is never moved or deleted by the tool. A mistake in a release is fixed by the next one.

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
