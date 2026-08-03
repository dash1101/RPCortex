#include "persist.h"
#include "registry.h"
#include "users.h"
#include "storage.h"

#include <stdlib.h>

// Config files are small; a few KB each is generous. The read helper caps at
// this, so a corrupted oversized file is truncated rather than eating the heap.
#define CFG_BUF 4096
// The OS's own data lives under /os, away from anything a user creates. The
// paths were at the root before; load_one falls back to the old name so an
// existing device keeps its accounts across the move, and the next save writes
// the new location.
#define REG_FILE       "/os/registry.cfg"
#define USERS_FILE     "/os/users.cfg"
#define REG_FILE_OLD   "registry.cfg"
#define USERS_FILE_OLD "users.cfg"

// Returns false when the file held nothing, so the caller can try the pre-/os
// location instead of loading an empty table over a perfectly good one.
static bool load_one(const char *file, void (*loader)(const char *, uint32_t)) {
    uint8_t *buf = (uint8_t *)malloc(CFG_BUF);
    if (!buf) return false;
    uint32_t n = storage_read_file(file, buf, CFG_BUF - 1);
    buf[n] = 0;
    if (n) loader((const char *)buf, n);
    free(buf);
    return n > 0;
}

void persist_load_all(void) {
    if (!load_one(REG_FILE, reg_load))     load_one(REG_FILE_OLD, reg_load);
    if (!load_one(USERS_FILE, users_load)) load_one(USERS_FILE_OLD, users_load);
}

static void save_one(const char *file, uint32_t (*ser)(char *, uint32_t)) {
    uint32_t need = ser(nullptr, 0);
    char *buf = (char *)malloc(need + 1);
    if (!buf) return;
    uint32_t n = ser(buf, need + 1);
    storage_write_file(file, (const uint8_t *)buf, n);
    free(buf);
}

void persist_save_registry(void) { save_one(REG_FILE, reg_serialize); reg_mark_clean(); }
void persist_save_users(void)    { save_one(USERS_FILE, users_serialize); users_mark_clean(); }

void persist_save_dirty(void) {
    if (reg_dirty())   persist_save_registry();
    if (users_dirty()) persist_save_users();
}
