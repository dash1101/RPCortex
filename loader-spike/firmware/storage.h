#ifndef RPC_STORAGE_H
#define RPC_STORAGE_H

#include <stdint.h>
#include "loader.h"

// littlefs v2 on the tail of the on-board flash. v2.11 — the same version and
// on-disk format MicroPython's rp2 port uses, so a v1.0 device's data is at
// least format-compatible with what v2.0 would mount.
bool     storage_init(bool format_if_needed);
// Erase and remake the filesystem, then mount it. The recovery path, used when
// mounting fails or the device has repeatedly failed to reach a shell.
bool     storage_format_and_mount(void);
bool     storage_write_file(const char *name, const uint8_t *data, uint32_t len);
// Append rather than truncate, for the shell's '>>' redirect. Creates the file
// if it does not exist.
bool     storage_append_file(const char *name, const uint8_t *data, uint32_t len);
// Read a whole file into buf. Returns bytes read, or 0 if absent/too big. Used
// for the small config files (registry, users), not for app images — those
// stream through storage_open_source so a large one never lands in RAM whole.
uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap);
bool     storage_open_source(const char *name, AppSource *src, void **handle);
void     storage_close_source(void *handle);

// Streaming WRITE, the mirror of storage_open_source. A download arrives a
// segment at a time and must never be held in RAM whole, and reopening the file
// per segment would commit littlefs metadata hundreds of times for one package
// — slow, and needless flash wear. The handle keeps the file open; the
// filesystem lock is taken per write rather than held across the transfer, so a
// slow download does not block every other task for its duration.
void    *storage_open_sink(const char *name);          // truncating create
bool     storage_sink_write(void *handle, const uint8_t *data, uint32_t len);
bool     storage_close_sink(void *handle);             // false if the tail failed
void     storage_list(void);

// VFS operations for the shell's filesystem commands. Paths are absolute
// (the shell resolves cwd + relatives before calling). All return false on
// failure rather than raising.
bool     storage_mkdir(const char *path);
bool     storage_remove(const char *path);              // file or empty dir
bool     storage_rename(const char *from, const char *to);
bool     storage_stat(const char *path, bool *is_dir, uint32_t *size);
bool     storage_copy(const char *from, const char *to);  // streamed, no whole-file RAM
// Walk one directory, calling cb per entry. Returns false if the dir can't open.
typedef void (*StorageWalkFn)(void *ctx, const char *name, bool is_dir, uint32_t size);
bool     storage_walk(const char *path, StorageWalkFn cb, void *ctx);
uint32_t storage_free_bytes(void);
uint32_t storage_total_bytes(void);   // the whole filesystem partition, for df

// Shorten a file to `size` bytes. Fails if the file is absent or shorter.
//
// Needed because a drive arrives in whole 512-byte sectors while a file ends
// wherever it ends: the last sector of a dropped file is padded, and the real
// length is only known from a directory entry that may arrive after the data.
bool     storage_truncate(const char *path, uint32_t size);

// Bumped by every operation that changes the filesystem.
//
// Something presenting a cached VIEW of the filesystem — the USB drive
// synthesises one — has no other way to notice that the thing it is describing
// moved underneath it. Comparing this against the value held when the view was
// built is cheap and cannot miss a change, which polling for one can.
uint32_t storage_generation(void);

// Flash the firmware occupies, and the region reserved for it. The filesystem
// starts at the reserve, so the first must stay below the second or an update
// would overwrite the start of the filesystem.
uint32_t storage_firmware_bytes(void);
uint32_t storage_reserve_bytes(void);

// --- the USB transfer area --------------------------------------------------
//
// A real FAT12 volume in its own flash region, NOT a view synthesised over
// littlefs. That distinction is the whole point: the host owns it outright and
// may create, edit, rename and delete in it exactly as it would on a memory
// card, because nothing here has to interpret what a sector write meant. The
// device reads it with a small FAT12 reader when it wants what is there.
//
// It shares the region an update is staged into. Both are scratch space and
// neither survives the other, which is why `update` clears it deliberately
// rather than writing over a volume the host may still have mounted.
uint32_t storage_usb_offset(void);
uint32_t storage_usb_bytes(void);

// Raw access to that region. Reads are memory-mapped, so they cost nothing;
// writes take a whole 4 KB erase block, because that is the unit flash erases
// in and a smaller write would mean reading, erasing and rewriting anyway.
bool     storage_usb_read(uint32_t off, void *buf, uint32_t len);
bool     storage_usb_write_block(uint32_t off, const uint8_t *block4k);
#define  STORAGE_USB_BLOCK 4096

// The firmware slot, and where an update is staged before it is applied.
//
// They exist because source and destination are one flash chip: an update reads
// the new image while erasing the old, and erasing the firmware takes littlefs
// with it. Staging puts the image at a fixed offset the final copy can read
// with no filesystem involved.
uint32_t storage_fw_slot_bytes(void);      // the most a firmware image may be
uint32_t storage_stage_offset(void);       // flash offset of the staging slot

// Copy a file into the staging slot, 4 KB at a time. Safe and interruptible:
// nothing that runs the device is touched. Returns the bytes staged, or 0.
// Called every sector, so a caller can show progress and — importantly — keep
// the watchdog fed. Staging 700 KB is around 174 erase-and-program cycles and
// several seconds; without something yielding in there the watchdog concludes
// the device has stopped and reboots mid-copy.
typedef void (*StorageProgressFn)(void *ctx, uint32_t done, uint32_t total);
uint32_t storage_stage_file(const char *path, StorageProgressFn cb, void *ctx);

// Last modification time as a Unix epoch, or 0 when it was never recorded (the
// clock had not been set when the file was written). Held as a littlefs custom
// attribute, since littlefs itself stores no timestamps.
uint32_t storage_mtime(const char *path);

#endif  // RPC_STORAGE_H
