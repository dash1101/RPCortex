# Releases

What a device and the websites read. Written by `tools/release.py`; nothing here is
edited by hand except `boards.json`.

| File | Read by | What it is |
|---|---|---|
| `latest.json` | a device, for `update check` | One entry per board with the size and SHA-256 of the raw image. The entries share their shape with a package index entry on purpose, so the same scanner reads both. |
| `rpcortex-v2-<board>.bin` | a device, for `update install` | The raw image, copied into flash directly. |
| `rpcortex-v2-<board>.uf2` | the boot ROM | The same image wrapped so drag and drop works: every 256 bytes in a 512-byte block with a header. Pure overhead for an update, which is why both exist. |
| `releases.json` | the websites | The history, newest first: version, build, date, summary, notes by section, and the assets with sizes and checksums. |
| `boards.json` | the websites, the release notes | Which boards have run which build. Edited by hand when a board is tested. |

`ver` in `latest.json` is `MAJOR.ITER.BUILD`, for example `2.0.486`, the same string
`RPC_OS_BUILDVER` compiles into the image. `repo_version_cmp` compares it number by number,
so a later build beats an earlier one and `2.10` follows `2.9`. The build is the commit count
at the time the images were built.

Release flow and how to write the notes: `RELEASING.md`.
