#include "persist.h"
#include "registry.h"
#include "users.h"
#include "storage.h"

#include <stdlib.h>
#include <stdio.h>

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

// --- the shadow copies ------------------------------------------------------
//
// These two files ARE the device. Lose the registry and every setting goes —
// the WiFi networks, the clock speed, the device name. Lose the accounts and
// there is no way to log in, and the only recovery is a first-run setup that
// throws away everything else with it.
//
// Both are a few kilobytes. Keeping a second copy costs nothing worth counting
// and turns "the settings are gone" into "the settings came back", which is the
// difference between a device that needs rescuing and one that rescues itself.
//
// THE SHADOW IS ONLY EVER WRITTEN FROM SOMETHING THAT LOADED. A copy taken from
// a file nobody has parsed is a copy of the damage, saved carefully — so the
// backup is written from the IN-MEMORY table after a successful load, not by
// copying bytes from one path to another.
#define REG_BACKUP     "/os/registry.bak"
#define USERS_BACKUP   "/os/users.bak"

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

static void save_one(const char *file, uint32_t (*ser)(char *, uint32_t)) {
    uint32_t need = ser(nullptr, 0);
    char *buf = (char *)malloc(need + 1);
    if (!buf) return;
    uint32_t n = ser(buf, need + 1);
    storage_write_file(file, (const uint8_t *)buf, n);
    free(buf);
}

// What the last load had to do. Read by fsinit so the boot can say so, because
// a device that quietly restored itself and said nothing has taught nobody
// that anything is wrong with the flash.
static PersistRepair g_repair;

const PersistRepair *persist_repair_report(void) { return &g_repair; }

// Load one table, and heal it if the primary is gone or unreadable.
//
// "Unreadable" is not only a missing file. A parse that yields NOTHING from a
// file that had bytes in it is the same outcome from the device's point of
// view, and it is what a truncated or half-written file looks like — so the
// count AFTER loading is the test, not the presence of the file.
static void load_healing(const char *file, const char *old_file, const char *backup,
                         void (*loader)(const char *, uint32_t),
                         uint32_t (*count)(void),
                         uint32_t (*ser)(char *, uint32_t),
                         bool *restored, bool *lost) {
    if (!load_one(file, loader)) load_one(old_file, loader);
    if (count() > 0) {
        // Good. Refresh the shadow from what is now in memory — which is the
        // only version that has been proven to parse.
        save_one(backup, ser);
        return;
    }

    // Nothing came back. Try the shadow before giving up on it.
    load_one(backup, loader);
    if (count() > 0) {
        *restored = true;
        save_one(file, ser);        // put the primary back so the next boot is clean
        return;
    }
    *lost = true;
}

void persist_load_all(void) {
    g_repair = PersistRepair{};
    load_healing(REG_FILE, REG_FILE_OLD, REG_BACKUP,
                 reg_load, reg_count, reg_serialize,
                 &g_repair.registry_restored, &g_repair.registry_lost);
    load_healing(USERS_FILE, USERS_FILE_OLD, USERS_BACKUP,
                 users_load, users_count, users_serialize,
                 &g_repair.users_restored, &g_repair.users_lost);
}

// Every save updates the shadow too. The pair is written primary-first: if
// power goes between them the shadow is one revision old, which is a setting
// lost rather than a device that cannot log in.
void persist_save_registry(void) {
    save_one(REG_FILE, reg_serialize);
    save_one(REG_BACKUP, reg_serialize);
    reg_mark_clean();
}
void persist_save_users(void) {
    save_one(USERS_FILE, users_serialize);
    save_one(USERS_BACKUP, users_serialize);
    users_mark_clean();
}

void persist_save_dirty(void) {
    if (reg_dirty())       persist_save_registry();
    if (users_dirty())     persist_save_users();
    if (reg_scope_dirty()) persist_save_scope();
}

// --- the signed-in user's own settings ----------------------------------------

// "/home/<user>/settings.cfg". Built here rather than from reg_note_home so the
// trailing slash question has one answer in one place.
static bool scope_path(const char *user, char *out, uint32_t cap) {
    if (!user || !user[0]) return false;
    // A name with a separator in it would write outside the home directory it
    // is supposed to be confined to. Accounts cannot contain one today; this is
    // here so that stays true if the rules for a name ever loosen.
    for (const char *p = user; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    snprintf(out, cap, "/home/%s/settings.cfg", user);
    return true;
}

void persist_save_scope(void) {
    const char *user = reg_scope_user();
    char path[64];
    if (!scope_path(user, path, sizeof(path))) return;
    // The home directory is made by fsinit at boot and again when an account is
    // created, but a factory reset wipes /home wholesale and the next save
    // would land nowhere. Cheap to be sure.
    char dir[64];
    snprintf(dir, sizeof(dir), "/home/%s", user);
    storage_mkdir(dir);
    save_one(path, reg_scope_serialize);
    reg_scope_mark_clean();
}

void persist_scope_enter(const char *user) {
    // Whoever was here before goes first, with their changes written out. A
    // login that replaced the scope without saving would silently discard the
    // previous session's settings.
    persist_scope_leave();
    if (!user || !user[0]) return;

    char path[64];
    if (!scope_path(user, path, sizeof(path))) return;
    reg_scope_load(nullptr, 0);              // start from nothing, not from the last user
    load_one(path, reg_scope_load);
    reg_scope_set(user);
    reg_scope_mark_clean();
}

void persist_scope_leave(void) {
    if (reg_scope_dirty()) persist_save_scope();
    reg_scope_clear();
}
