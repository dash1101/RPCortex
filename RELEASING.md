# Releasing

How a change becomes a release, and how a release reaches the devices, GitHub and the
websites. Everything here is driven by `tools/release.py`; `tools/release.py --help` lists
its commands.

## Versions

`vMAJOR.ITER`, with the build number beside it: `v2.11 (build 1203)`.

- **MAJOR** is a generation. It is the only thing with a codename (v1 Vela, v2 Vela II) and it
  changes when the platform does: a new kernel, a new package ABI, a new architecture. The first
  release of a generation is `v2.00`.
- **ITER** counts the patch releases within a generation, from zero, written with two digits, with
  no ceiling. v2.09 is followed by v2.10. A patch never breaks packages. A change that would is a
  new MAJOR.
- **-beta** marks a pre-release: `v2.00-beta (build 486)`. It is dropped when the version ships.
- **The build number** is the commit count when the images were built. It rises on its own and is
  never typed by hand. Devices compare `MAJOR.ITER.BUILD` (`2.00.486`), so a later build always
  beats an earlier one.

**Builds are not releases.** A patch is cut on a schedule, when there is enough to say, and the
builds in between are not published. That keeps the schedule steady and the work per release small:
the unit of work is the changelog entry, written as the change lands, not a release per commit.

The version lives in one place, `os/kernel/kernel.h`. `tools/release.py bump iter`,
`bump major "Name"` and `bump ship` edit it.

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
   - commits, tags (`v2.11`, or `v2.00-beta.486` for a pre-release), pushes, and creates the GitHub release.

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
- A pre-release is cut as often as is useful; each gets its own tag. There is no moving "latest
  beta" tag.
- A release is cut from a commit that passes CI. The tool refuses a dirty tree.
- Retiring the old `v2.0.0` pre-release tag, which predates this scheme, is a separate step:
  `gh release delete v2.0.0 --cleanup-tag`, done once the first new-scheme release is published.
