#include "persist.h"
#include "registry.h"
#include "users.h"
#include "storage.h"

#include <stdlib.h>

// Config files are small; a few KB each is generous. The read helper caps at
// this, so a corrupted oversized file is truncated rather than eating the heap.
#define CFG_BUF 4096
#define REG_FILE   "registry.cfg"
#define USERS_FILE "users.cfg"

static void load_one(const char *file, void (*loader)(const char *, uint32_t)) {
    uint8_t *buf = (uint8_t *)malloc(CFG_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(file, buf, CFG_BUF - 1);
    buf[n] = 0;
    loader((const char *)buf, n);
    free(buf);
}

void persist_load_all(void) {
    load_one(REG_FILE, reg_load);
    load_one(USERS_FILE, users_load);
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
