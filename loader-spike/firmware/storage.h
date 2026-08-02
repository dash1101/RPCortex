#ifndef RPC_STORAGE_H
#define RPC_STORAGE_H

#include <stdint.h>
#include "loader.h"

// littlefs v2 on the tail of the on-board flash. v2.11 — the same version and
// on-disk format MicroPython's rp2 port uses, so a v1.0 device's data is at
// least format-compatible with what v2.0 would mount.
bool     storage_init(bool format_if_needed);
bool     storage_write_file(const char *name, const uint8_t *data, uint32_t len);
// Read a whole file into buf. Returns bytes read, or 0 if absent/too big. Used
// for the small config files (registry, users), not for app images — those
// stream through storage_open_source so a large one never lands in RAM whole.
uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap);
bool     storage_open_source(const char *name, AppSource *src, void **handle);
void     storage_close_source(void *handle);
void     storage_list(void);
uint32_t storage_free_bytes(void);

#endif  // RPC_STORAGE_H
