# Releases

`latest.json` is the manifest a device reads for `update check`. It lists one
entry per board, with the size and SHA-256 of the raw image.

The entries share their shape with a package index entry on purpose, so the same
scanner reads both. A second format would be a second parser to get wrong.

`ver` is the frozen version with the build number as a fourth component —
`2.0.0.<build>`, the same string `RPC_OS_BUILDVER` compiles into the image.
`repo_version_cmp` counts a missing component as zero, so a later build beats an
earlier one and a frozen version needs no special case. Given no argument,
`make-release.py` derives that string from the commit count rather than asking
for it; an explicit version on the command line overrides it.

BOTH FORMATS, for two different jobs.

The `.bin` is the raw image and is what an over-the-air update writes: `latest.json`
points at it, and the device copies it into flash directly.

The `.uf2` is the same image wrapped so the boot ROM will accept it — every 256
bytes in a 512-byte block with a header. That wrapping is exactly what makes
drag-and-drop work and is pure overhead for an update, which is why both are
here rather than one. The download picker on the site links to the `.uf2`; it
linked here before either existed, and every button on that page was a 404.

Rebuild the manifest after a release with `tools/make-release.py`.
