# Releases

`latest.json` is the manifest a device reads for `update check`. It lists one
entry per board, with the size and SHA-256 of the raw image.

The entries share their shape with a package index entry on purpose, so the same
scanner reads both. A second format would be a second parser to get wrong.

Images are the raw `.bin`, not the `.uf2`. A `.uf2` wraps every 256 bytes in a
512-byte block with a header, which is what the boot ROM's drag-and-drop needs
and pure overhead for an update that writes flash directly.

Rebuild the manifest after a release with `tools/make-release.py`.
