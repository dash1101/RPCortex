#include "registry.h"
#include "lock.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// The scope is an account name, so it is bounded by the same thing accounts are.
#define USR_SCOPE_MAX 25

struct Entry {
    char key[REG_KEY_MAX];
    char val[REG_VAL_MAX];
};

// A background task calling reg_set while the shell is serializing the table to
// flash writes a half-old, half-new file. One lock covers both sides.
static RpcLock g_reg_lock;
static Entry   g_reg[REG_MAX];
static uint32_t g_count;
static bool     g_dirty;

// --- the per-user table -------------------------------------------------------
// See the long note in registry.h for which keys live here and why it is not
// simply "everything an app owns".

static Entry    g_user[REG_USER_MAX];
static uint32_t g_user_count;
static bool     g_user_dirty;
static char     g_scope[USR_SCOPE_MAX];

bool reg_is_user_key(const char *key) {
    return key && strncmp(key, "User.", 5) == 0;
}

const char *reg_scope_user(void) { return g_scope; }

static int find(const char *key) {
    for (uint32_t i = 0; i < g_count; i++)
        if (strcmp(g_reg[i].key, key) == 0) return (int)i;
    return -1;
}

static int find_user(const char *key) {
    for (uint32_t i = 0; i < g_user_count; i++)
        if (strcmp(g_user[i].key, key) == 0) return (int)i;
    return -1;
}

// Where a read should look first. Null when this key is not the signed-in
// user's business, or when they have never set it.
static const Entry *scoped(const char *key) {
    if (!g_scope[0] || !reg_is_user_key(key)) return nullptr;
    int i = find_user(key);
    return i >= 0 ? &g_user[i] : nullptr;
}

const char *reg_get(const char *key, const char *def) {
    LockGuard _r(&g_reg_lock);
    if (const Entry *e = scoped(key)) return e->val;
    // Falling through to the device table is deliberate and is what makes this
    // survivable: a "User." key set device-wide is the value everybody starts
    // from, so a setting that becomes per-user keeps working for people who
    // never touch it again.
    int i = find(key);
    return i >= 0 ? g_reg[i].val : def;
}

int32_t reg_get_int(const char *key, int32_t def) {
    const char *v = reg_get(key, nullptr);
    if (!v) return def;
    char *end = nullptr;
    long n = strtol(v, &end, 10);
    return (end && end != v) ? (int32_t)n : def;
}

// Put a value in the DEVICE table. Assumes the lock is held and the lengths
// have been checked.
//
// Split out of reg_set because reg_load must NOT go through reg_set's refusal.
// A "User." key sitting in the device file is the DEFAULT everybody starts from
// — the thing that makes a setting survive becoming per-user at all — and the
// refusal is about a live caller writing one with nobody signed in. Routing the
// loader through it meant the device file could hold such a default and the
// device could never read one back, which is a file that quietly does nothing.
static bool device_put(const char *key, const char *value) {
    int i = find(key);
    if (i < 0) {
        if (g_count >= REG_MAX) return false;
        i = (int)g_count++;
        strcpy(g_reg[i].key, key);
    }
    strcpy(g_reg[i].val, value);
    g_dirty = true;
    return true;
}

bool reg_set(const char *key, const char *value) {
    LockGuard _r(&g_reg_lock);
    if (!key || !value) return false;
    if (strlen(key) >= REG_KEY_MAX || strlen(value) >= REG_VAL_MAX) return false;

    if (reg_is_user_key(key)) {
        // REFUSED rather than written device-wide.
        //
        // Falling back to the device table here would mean a background service
        // that runs before anyone logs in — the Nova D1's screen does exactly
        // that — silently setting the default every later account inherits. A
        // preference quietly becoming device policy is the failure this whole
        // split exists to avoid, so it is better to lose the write and let the
        // caller's own default stand.
        if (!g_scope[0]) return false;
        int i = find_user(key);
        if (i < 0) {
            if (g_user_count >= REG_USER_MAX) return false;
            i = (int)g_user_count++;
            strcpy(g_user[i].key, key);
        }
        strcpy(g_user[i].val, value);
        g_user_dirty = true;
        return true;
    }

    return device_put(key, value);
}

bool reg_has(const char *key) { return scoped(key) != nullptr || find(key) >= 0; }
uint32_t reg_count(void) { return g_count; }
const char *reg_key_at(uint32_t i) { return i < g_count ? g_reg[i].key : nullptr; }

void reg_clear(void) { g_count = 0; g_dirty = true; }

void reg_load(const char *text, uint32_t len) {
    LockGuard _r(&g_reg_lock);
    g_count = 0;
    if (!text) { g_dirty = false; return; }
    uint32_t i = 0;
    char line[REG_KEY_MAX + REG_VAL_MAX + 2];
    while (i < len) {
        uint32_t j = 0;
        while (i < len && text[i] != '\n' && j < sizeof(line) - 1) line[j++] = text[i++];
        while (i < len && text[i] != '\n') i++;   // discard an over-long tail
        if (i < len) i++;                          // step past the newline
        line[j] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        // device_put rather than reg_set: see the note on device_put for why
        // the loader must not meet the setter's refusal. The caps are checked
        // here instead, so a malformed file still cannot corrupt the table.
        if (strlen(line) >= REG_KEY_MAX || strlen(eq + 1) >= REG_VAL_MAX) continue;
        LockGuard _l(&g_reg_lock);
        device_put(line, eq + 1);
    }
    g_dirty = false;   // a freshly loaded table matches the file on disk
}

uint32_t reg_serialize(char *buf, uint32_t cap) {
    LockGuard _r(&g_reg_lock);
    uint32_t need = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        int n = snprintf(nullptr, 0, "%s=%s\n", g_reg[i].key, g_reg[i].val);
        if (n < 0) continue;
        if (buf && need + (uint32_t)n < cap)
            snprintf(buf + need, cap - need, "%s=%s\n", g_reg[i].key, g_reg[i].val);
        need += (uint32_t)n;
    }
    if (buf && cap) buf[need < cap ? need : cap - 1] = 0;
    return need;
}

bool reg_dirty(void) { return g_dirty; }
void reg_mark_clean(void) { g_dirty = false; }

// --- the per-user table, the rest of it ---------------------------------------

void reg_scope_set(const char *user) {
    LockGuard _r(&g_reg_lock);
    snprintf(g_scope, sizeof(g_scope), "%s", user ? user : "");
}

void reg_scope_clear(void) {
    LockGuard _r(&g_reg_lock);
    g_user_count = 0;
    g_user_dirty = false;
    g_scope[0] = 0;
}

void reg_scope_load(const char *text, uint32_t len) {
    {
        LockGuard _r(&g_reg_lock);
        g_user_count = 0;
    }
    if (!text) { g_user_dirty = false; return; }
    uint32_t i = 0;
    char line[REG_KEY_MAX + REG_VAL_MAX + 2];
    while (i < len) {
        uint32_t j = 0;
        while (i < len && text[i] != '\n' && j < sizeof(line) - 1) line[j++] = text[i++];
        while (i < len && text[i] != '\n') i++;    // discard an over-long tail
        if (i < len) i++;
        line[j] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        // Only the keys that belong here. A file that has picked up a device
        // key — by hand, or from a version of this that routed differently —
        // must not be able to shadow one, because a read prefers this table and
        // there would be no way to see which value was in force.
        if (!reg_is_user_key(line)) continue;
        LockGuard _r(&g_reg_lock);
        if (g_user_count >= REG_USER_MAX) break;
        if (strlen(line) >= REG_KEY_MAX || strlen(eq + 1) >= REG_VAL_MAX) continue;
        int at = find_user(line);
        if (at < 0) { at = (int)g_user_count++; strcpy(g_user[at].key, line); }
        strcpy(g_user[at].val, eq + 1);
    }
    g_user_dirty = false;
}

uint32_t reg_scope_serialize(char *buf, uint32_t cap) {
    LockGuard _r(&g_reg_lock);
    uint32_t need = 0;
    for (uint32_t i = 0; i < g_user_count; i++) {
        int n = snprintf(nullptr, 0, "%s=%s\n", g_user[i].key, g_user[i].val);
        if (n < 0) continue;
        if (buf && need + (uint32_t)n < cap)
            snprintf(buf + need, cap - need, "%s=%s\n", g_user[i].key, g_user[i].val);
        need += (uint32_t)n;
    }
    if (buf && cap) buf[need < cap ? need : cap - 1] = 0;
    return need;
}

bool reg_scope_dirty(void) { return g_user_dirty; }
void reg_scope_mark_clean(void) { g_user_dirty = false; }
uint32_t reg_scope_count(void) { return g_user_count; }
const char *reg_scope_key_at(uint32_t i) { return i < g_user_count ? g_user[i].key : nullptr; }
