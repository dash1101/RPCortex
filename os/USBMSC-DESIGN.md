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

## The drive is enumerated three seconds before the filesystem is mounted

Worth its own heading because the first build hit it, and because the symptom
carried no information at all.

`main` brings up stdio and then waits up to three seconds for a host, driving
the device stack so enumeration can complete. `kboot` — which is where
`storage_init` mounts littlefs — runs *after* that. So a device plugged in at
power-on is fully enumerated, and answering SCSI commands, while it still has no
filesystem.

Nothing in the first version noticed. `storage_total_bytes()` is a compile-time
constant, so the geometry came out correct; `storage_walk("/")` returned false
because nothing was mounted, and added no entries; and `msc_build()` reported
success anyway, because the only failure it recognised was a failed allocation.
The result — a valid, correctly sized, entirely empty volume — was then cached
as ready for the rest of the run.

What the host showed: a drive that mounts cleanly, reports the right size, is
write-protected as intended, and contains nothing. What the device showed: no
error, anywhere.

Two things fix it, and both are needed:

- **Not ready until the filesystem is mounted.** `storage_stat("/")` is the
  probe, since it returns false when nothing is mounted. Reporting not-ready is
  also what the host wants: the MSC layer answers "medium not present", TinyUSB
  advertises the device as removable, and every operating system polls
  removable media rather than giving up on it.
- **An unreadable root is a failure, not an empty drive.** They produce an
  identical node table, so the two have to be told apart where the difference
  is still known — at the walk.

The general lesson is the one worth keeping: **a synthesised view has no error
path of its own.** Anything that goes wrong while building it is indistinguishable
from a device that genuinely has nothing on it, so every step that can fail has
to say so at the point of failure. `usbdrive status` exists for the same reason
— it prints the table, which is the only way to tell "empty" from "broken"
without a rebuild.

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
- **The two locks are taken in opposite orders.** The shell prints while
  holding the filesystem lock — `ls` prints each entry from inside
  `storage_walk`, which holds it for the whole walk — so its order is
  `g_fs_lock` then the SDK's `stdio_usb_mutex`. Anything reached through
  `tud_task()` is inside `stdio_usb_mutex` already and takes `g_fs_lock`
  second. Overlap those and neither side moves until the SDK's mutex gives up
  after a second, and it gives up by discarding the console output that was
  waiting. Both tasks are pinned to core 0, so this is reachable rather than
  theoretical.

  The tree walk is the long hold, and it has been moved onto the `usb` task
  before it enters the device stack, which takes the dominant window out of
  the inversion. What is left inside is a single sector read — on this
  filesystem a memcpy out of memory-mapped flash — so the remaining exposure
  is a narrow one, and the symptom if it is ever hit is a second of stalled
  console and some lost output rather than anything corrupted.

  It is still an inversion and it should be closed properly. The honest fix is
  that `storage_walk` should not hold the filesystem lock across a caller's
  callback, since that callback can do anything at all, including print.

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

---

# The rewrite: a real volume, not a view of one

Everything above describes synthesising a FAT view over littlefs. That approach
was built, shipped to hardware, and abandoned. What follows is why, because the
reasoning is the useful part and the failures were not obvious in advance.

## What went wrong, in order

Each of these was found on a device, and none of them showed up as an error.

1. **Files dropped from the host never arrived.** The count of occupied root
   directory slots used the size of the whole node table rather than the root's
   own children, so the free slots a host writes a new entry into were declared
   occupied. Separately, Windows writes a file's data BEFORE the entry naming
   it, and collection only began when a name appeared — so the data was
   discarded and the entry then described contents already thrown away.
2. **Changes made on the device never reached the host.** The view was
   invalidated and rebuilt two statements apart, so the not-ready state no host
   could miss lasted microseconds and every host missed it.
3. **Edits never stuck, in either direction.** By design: an edit arrives as a
   write over a region describing an existing file, which the view had to refuse
   because it could not tell an edit from a rename from a deletion.
4. **A three kilobyte copy locked the device up.** Honouring a write meant
   calling littlefs from inside the USB stack, which put the filesystem lock
   inside the console's mutex. The shell prints while holding the filesystem
   lock. Two orders, one core, no way out.

## Why they were not really four problems

A synthesised view has to infer intent from raw sector writes. A host never says
"create this file" — it writes clusters, table entries and a directory entry, in
whatever order it likes, and something underneath has to work out what was
meant. That can only ever recognise the cases it was taught, so anything else
gets refused, and refusing is indistinguishable from a bug to whoever is
watching. Meanwhile every case it DOES honour has to reach the real filesystem
from inside the USB stack, which is where the deadlock lives.

The desynchronisation was not a bug to be fixed either. There were two copies of
the truth — littlefs, and whatever the host had cached — and no way to tell the
host that a sector it already read had changed.

## What replaced it

A real FAT12 volume in its own flash region, owned by the host.

Nothing interprets anything: a sector write is a sector write. Creating,
renaming, editing and deleting all work because the host is doing them to a
filesystem rather than to a description of one. There is one copy of the data
and both sides read the same bytes, so there is nothing to fall out of step. And
because the region is not littlefs, the MSC path never takes the filesystem
lock, which removes the deadlock by construction rather than by care.

The cost is real and worth stating: the host sees a transfer area, not the
device's filesystem. Moving a file between them is `usb get` or `usb put`.

## The details that decided the shape

- **The region is the firmware staging slot.** It is the only megabyte on the
  chip not already spoken for, and using it means littlefs does not move —
  which matters because moving where the filesystem starts costs every existing
  device its files. The price is that staging an update overwrites the area, so
  `update` gives it up deliberately and says so rather than letting a firmware
  image and a file drop interleave.
- **FAT12, not FAT16.** The format is decided by the cluster count and the
  boundary is exact: under 4085 clusters a volume IS FAT12. A megabyte at
  512-byte clusters is about 2033, so there is no choice, and the padding trick
  the synthesised view used to stay above the line is not available at this
  size. Twelve-bit entries pack two into three bytes, half of them straddling a
  byte boundary and one in every 341 straddling a sector boundary.
- **A 4 KB write-back cache.** Flash erases in 4 KB blocks and a host writes in
  512-byte sectors. The flush happens on the usb task BEFORE it enters the
  device stack, not inside a callback, so an erase never happens with the
  console's mutex held.
- **Nothing prints while holding the drive's lock.** The console path takes the
  stdio mutex, the usb task holds that mutex while servicing the device stack,
  and the drive's lock is taken on both sides of it. This is the same shape as
  the deadlock that was just removed, one level down, and the only defence is
  the discipline: gather under the lock, print after.
- **The callbacks give up rather than spin.** Returning zero asks TinyUSB to
  call back, but tud_task loops until its queue is empty, so the retry happens
  inside the same call without yielding — and if the lock holder is a task on
  the same core it never runs. The retry is bounded; past the limit the
  operation fails and the host tries again later. A failed read is recoverable
  and a hung one is not.

## Off by default, and the inbox

Two changes after the first working version, both from using it.

**The drive is off unless asked for.** `usb on` offers it, `usb off` withholds
it, `usb auto` offers it whenever a host has actually configured the device and
withholds it on a charger or a battery. The setting persists. Off is the default
because plugging a device into a machine should not hand that machine a
filesystem by reflex, and the person doing the plugging is not always the person
who owns it. Being honest about `auto`: on a board that lives plugged into a
computer it is on nearly all the time, and the distinction it draws is narrow.

**Anything dropped on the drive is copied into `/usb`.** A file sitting in the
transfer area was not much use — the area is FAT and everything else on the
device is littlefs, so nothing else could read it. Now a scan runs on the usb
task once the host has been quiet for a second and a half, and copies what it
finds into `/usb`, where `cat`, `pkg`, the editor and every other command can
reach it.

Copied rather than moved. A file that vanished from the drive the moment it
finished transferring would look like a failure, and the host has it cached as
present anyway. Nothing is imported twice: a name already in `/usb` at the same
size is left alone, which makes the scan idempotent — it has to be, because it
runs on a timer rather than on an event. MSC has no "the copy has finished"
signal beyond a cache flush and not every host sends one, so quiet is the only
reliable indication that a file is whole.

**When the filesystem is full the drive goes read-only.** The transfer area has
its own free space and the host respects it, but a file is only useful once it
has been copied off, and the filesystem can run out first. Accepting a write
that has nowhere to go is worse than refusing it, because the host believes the
file arrived. So a failed import sets write protection, and it clears by itself
on the next scan once something has been deleted — a latch needing to be cleared
by hand would be one more thing to remember at exactly the wrong moment.

Refused per command as well as through `tud_msc_is_writable_cb`: hosts read that
flag when they mount the volume and do not necessarily look again, so a device
that fills up mid-session would otherwise keep accepting files it cannot keep.

## The drive exists only during `download`

The off/on/auto setting is gone, and so is the `usb` command. There is one
command and the drive exists while it is running.

That is the security model as well as the interface. A setting can be left
switched on and then forgotten; a mode cannot, because "is the filesystem
exposed right now" has exactly one answer and it is on the screen. A device
plugged into someone else's machine offers a serial console and nothing else,
because there is nobody at the prompt to have asked for more.

Formatted on the way IN, not out. A session that ended in a reset or a pulled
cable cannot leave yesterday's files on a drive somebody else plugs in, and
every session starts from the same state. It costs three flash blocks.

Both directions, one command:

    download                 open it empty, take what lands
    download report.txt      put that on it first, to be dragged off

Which is why there is no `usb put` any more: naming a file was the only thing
the second command did that this one does not.
