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

## Descriptors: a supported switch, not a rewrite

An earlier reading of this concluded that `pico_stdio_usb` owns the USB
descriptors with strong symbols, so adding an interface meant replacing the
whole USB layer. That is wrong, and the correction is worth stating plainly
because it changes the size of the task from days to hours.

The descriptor file is conditional on a documented config flag:

```c
// sdk/src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c:34
#if PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS
```

It defaults to 1 only when the application is not using TinyUSB directly, and it
is a plain `#ifndef` in `pico/stdio_usb.h` — so defining it to 0 compiles the
SDK's descriptors out and leaves the rest of `pico_stdio_usb` in place: the
stdio driver, `tusb_init()`, connection tracking, all of it. Supplying a
composite descriptor set is then the supported path rather than a fight with the
linker.

The MSC class needs no vendoring either. `class/msc/msc_device.c` is compiled
into the TinyUSB device library unconditionally and its body is guarded by
`CFG_TUD_MSC`, which is `#ifndef`-defaulted to 0 in `tusb_option.h`.

So the whole USB-layer question comes to four definitions and one new file:

```
PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
CFG_TUD_MSC=1
CFG_TUD_MSC_EP_BUFSIZE=512
PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0   # see "Where tud_task runs"
```

### Keeping the BOOTSEL reset interface

`PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE` also defaults to 1 here, so the
device already presents a vendor interface at ITF 2 that `picotool` uses to
reboot a board into BOOTSEL. Custom descriptors would silently drop it, and on a
board that gets reflashed as often as this one that is a real loss.

`pico/usb_reset.h` documents the recipe. Because `pico_stdio_usb` stays linked,
every step of it is already satisfied except one: the configuration descriptor
has to carry `TUD_RPI_RESET_DESCRIPTOR(2, str)` itself. The BOS descriptor, the
Microsoft OS 2.0 descriptor and the app driver callback all still come from the
SDK, and `PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF` is 2 — which fixes the reset
interface at ITF 2 and puts MSC at ITF 3.

Interface layout:

| ITF | What | Endpoints |
|---|---|---|
| 0, 1 | CDC — the console | `0x81` notify, `0x02` out, `0x82` in |
| 2 | Vendor reset — `picotool` | none |
| 3 | MSC — the drive | `0x03` out, `0x83` in |

The product ID changes with the interface set. Windows caches the interface
layout against VID:PID, and reusing the SDK's `2E8A:0009` for a device that now
has a fourth interface is a well-known way to get a machine that will not
enumerate it until the cache is cleared by hand.

## Where tud_task runs, and why it had to move

This is the constraint that actually shapes the code, and it is not obvious from
either the SDK or the TinyUSB documentation.

By default `pico_stdio_usb` calls `tud_task()` from a low-priority user IRQ, so
that USB keeps servicing whatever the application is doing. Every MSC callback
would therefore run **in interrupt context** — where `g_fs_lock` cannot be taken,
because `lock_acquire` yields, and where `flash_safe_execute` cannot run at all.

The obvious escape is the deferral the MSC API appears to offer: `read10` and
`write10` may return 0 for "not ready", and TinyUSB calls them again. It does not
work here. `tud_task_ext` loops until its event queue is empty, and returning 0
queues the retry immediately:

```c
} else if (nbytes == 0) {
  // zero means not ready -> simulate an transfer complete so that this driver callback will fired again
  dcd_event_xfer_complete(rhport, p_msc->ep_in, 0, XFER_RESULT_SUCCESS, false);
}
```

So the callback is re-entered inside the same interrupt, forever, while the
worker task that was supposed to satisfy it never gets the core back. Scheduling
here is cooperative — preemption is a stall-killer, not a time-slicer — so that
is a hang, not a slow path.

**There is no non-blocking deferral in the MSC read API.** Every design that
leaves `tud_task()` in the interrupt dies on that one callback.

So `tud_task()` moves to thread context: `PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0`
and a `usb` task drives it. `stdio_flush()` is what the task calls — it reaches
`tud_task()` through the stdio driver's `out_flush`, which means the SDK's own
`stdio_usb_mutex` still serialises it against the `printf` and `getchar` paths
that also call `tud_task()`. Nothing is reimplemented and nothing is re-entered.

The cost is that USB is now only serviced while that task runs, so anything that
blocks the shell for a long time without yielding also stalls enumeration. That
is the one regression to watch for on hardware, and it is checked before any FAT
code is written.

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

Settled: the synthesised view for reads, with writes limited to new files. See
below.

## The shape agreed

Show everything, accept only new files. That is not a compromise between the two
options above — it is most of what was wanted, at a fraction of the risk.

**Reads: the whole tree, synthesised.** Walk littlefs from the root and build a
FAT16 boot sector, FAT and directory from it. File data is served straight
through `lfs_file_read` when the host asks for the cluster it was told about.
Everything on the device is visible and copyable. Subdirectories are directory
entries pointing at more synthesised directory clusters; the same walk produces
them. If an SD card is ever mounted at `/SD` it is one more subtree in the same
walk and needs no separate thought — though nothing mounts one today, so that is
a prerequisite rather than a feature.

**Writes: only "a new file appeared".** The host creating a file writes three
things — a 32-byte directory entry, a chain in the FAT, and the data clusters.
Those can be recognised without implementing FAT semantics: buffer the cluster
writes, read the name and length out of the directory entry, and once the host
has stopped, hand the assembled bytes to `lfs_file_write`. This is the shape
CircuitPython and similar firmwares use.

**Everything else is refused.** Deleting, renaming, editing in place and
overwriting all arrive as sector writes over regions that describe existing
files, and honouring them is the filesystem driver this is avoiding. The volume
presents existing files read-only and says so, rather than accepting a change
and losing it.

That boundary is the whole design. It has to be enforced by WHERE a write lands
— inside the region describing existing files, or in free space — not by
guessing what the host meant.

## Things that are certain to happen on the first plug

Not risks — behaviours every desktop OS has, which the write path meets
immediately.

**The host writes its own files.** Windows creates `System Volume Information`,
macOS creates `.Spotlight-V100`, `.fseventsd` and `.Trashes`, and both do it
within seconds of mounting. A write path built to "accept new files" accepts
those too, and littlefs fills with host bookkeeping nobody asked for. The
refuse-list and a flat refusal to create directories are part of the first
working version, not a later polish pass.

**"The host has stopped" is not a signal.** There is no end-of-copy event in
MSC. The one that exists is SCSI `SYNCHRONIZE_CACHE` (0x35), which arrives
through `tud_msc_scsi_cb` at the end of a write and again on eject — so that is
what commits a buffered file. Nor does the directory entry reliably arrive
before the data: hosts commonly write it with size 0 and rewrite it afterwards,
so the assembled bytes cannot be committed on the strength of the first entry
seen.

**Existing files are marked read-only in the directory entry.** `ATTR_READ_ONLY`
on every synthesised entry is what makes a refused edit show up as the host
declining in its own interface, rather than as a write this code silently
discards.

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
  while the drive is mounted. `g_fs_lock` covers the OS side and the MSC
  callbacks take it too — which is only legal because they now run in thread
  context.

That last one is the reason to be careful rather than quick: the failure mode is
a corrupted filesystem, which on this device has previously meant a board that
would not boot until it was erased.

## The one open decision: buffered or streamed

A dropped file has to get from cluster writes into littlefs, and there are two
shapes with very different limits.

**Buffer the whole file in RAM**, commit on `SYNCHRONIZE_CACHE`. Simple, and the
size limit is whatever the heap can spare — a few hundred kilobytes at best on a
device with ~370 KB free, which a firmware image or a WAV file exceeds.

**Stream to a temporary littlefs file** as clusters arrive and rename it on
commit. RAM stays at one sector, and the limit becomes free flash instead. This
is legal now for exactly the reason above: the callbacks are in thread context,
so they may write flash. The cost is that an abandoned copy leaves a temp file
to clean up, and out-of-order cluster writes need seeking rather than appending.

Streaming is the answer, because the buffered version's limit is low enough to
be hit by the first genuinely useful thing anyone drags across. Whichever ships
first, the limit goes in the documentation rather than being discovered when a
1 MB drop fails.
