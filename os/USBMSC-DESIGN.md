# Drag-and-drop file transfer — what the reading turned up

Working notes for task #69. Delete or fold into ARCHITECTURE.md once it exists.

## The goal

Plug the device in and have a drive appear, so getting a file onto it is a drag
rather than `put <name> <len>` over the console or a round trip through the
package repository.

## What is already true

- **TinyUSB's MSC device class ships with the SDK** — `sdk/lib/tinyusb/src/class/msc/msc_device.c`.
  Nothing has to be vendored. It is a callback interface: the OS answers
  `tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_inquiry_cb` and a few
  others, in 512-byte blocks.
- The console is USB CDC via `pico_enable_stdio_usb`, with 1 KB buffers each
  way (`os/CMakeLists.txt`).
- The filesystem is littlefs over 2 MB of flash, mounted at boot, and every
  flash operation goes through `flash_safe_execute` — which parks the other
  core. Both cores are now registered as lockout victims.

## The pivotal constraint, and it is not the FAT layer

**`pico_stdio_usb` owns the USB descriptors, and its callbacks are not weak.**

`sdk/src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c` defines
`tud_descriptor_device_cb`, `tud_descriptor_configuration_cb` and
`tud_descriptor_string_cb` as ordinary strong symbols, and the file is
unconditionally part of the library. Adding a second USB interface means
supplying a composite descriptor set — and supplying one alongside those is a
duplicate-symbol link error, not an override.

So the first decision is not about FAT at all. It is one of:

1. **Stop linking `pico_stdio_usb` and provide the whole USB layer.** Use
   `tinyusb_device` directly, write the composite CDC+MSC descriptors, and
   register a `stdio_driver_t` for the CDC half so `printf` still reaches the
   console. This is the SDK's own structure minus the descriptor file, and it is
   how every composite-device example does it. Costs: the reset-via-vendor
   interface and the SDK's connection tracking would have to be carried over or
   dropped deliberately.

2. **Patch the SDK copy.** Fastest, and wrong — this tree vendors the SDK, and a
   local change to it is invisible to anyone building from a clean checkout.

(1) is the answer. It is worth knowing it costs a day of USB plumbing before any
of the interesting work starts.

## Then the actual problem: littlefs is not FAT

A host that mounts the partition raw sees an unformatted drive. The two ways:

- **A FAT16 view synthesised on the fly.** `tud_msc_read10_cb` answers a
  request for LBA 0 with a boot sector built from nothing, LBAs 1..n with a FAT
  built from the littlefs listing, then a root directory, then file data read
  through littlefs. Writes are the hard half: the host writes a directory entry
  and cluster chain, and something has to turn that back into
  `lfs_file_write`. Deleting, renaming and overwriting all arrive as raw sector
  writes that have to be interpreted.
- **A dedicated drop-box region** — a real FAT16 partition in its own flash
  range, with the OS copying anything that appears into littlefs. Far simpler
  and far less clever; the cost is that the drive shows only what has been
  dropped, not what is on the device.

The user chose the synthesised view. Worth re-confirming with the write side
laid out, because that is where the work is: a read-only synthesised view is an
afternoon, and read-write is a filesystem driver.

## Hazards worth writing down now

- **The host caches.** An OS that has mounted a FAT volume will not re-read
  sectors it thinks it knows. Files created on the device while the drive is
  mounted may not appear until it is ejected and re-plugged, and there is no
  standard way to say "this changed". Some designs deliberately drop the
  volume and re-present it to force a rescan.
- **Writes are 512-byte sectors and flash erases in 4 KB blocks**, so every
  sector write is a read-modify-write of a littlefs block under
  `flash_safe_execute`. That parks the other core. A host copying a large file
  will do this hundreds of times in a row.
- **The console shares the bus.** MSC traffic and CDC traffic interleave on one
  device; a long write must not starve the shell, and the shell must not stall
  the transfer.
- **Two writers, one filesystem.** The host and the OS can both want to write
  while the drive is mounted. `g_fs_lock` covers the OS side; the MSC callbacks
  have to take it too, and they run from USB interrupt context.

That last one is the reason to be careful rather than quick: the failure mode is
a corrupted filesystem, which on this device has previously meant a board that
would not boot until it was erased.
