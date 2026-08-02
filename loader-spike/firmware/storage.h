#ifndef RPC_STORAGE_H
#define RPC_STORAGE_H

#include <stdint.h>
#include "loader.h"

// littlefs v2 on the tail of the on-board flash. v2.11 — the same version and
// on-disk format MicroPython's rp2 port uses, so a v1.0 device's data is at
// least format-compatible with what v2.0 would mount.
bool     storage_init(bool format_if_needed);
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

// Last modification time as a Unix epoch, or 0 when it was never recorded (the
// clock had not been set when the file was written). Held as a littlefs custom
// attribute, since littlefs itself stores no timestamps.
uint32_t storage_mtime(const char *path);

#endif  // RPC_STORAGE_H
