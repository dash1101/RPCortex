#include "registry.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct Entry {
    char key[REG_KEY_MAX];
    char val[REG_VAL_MAX];
};

static Entry   g_reg[REG_MAX];
static uint32_t g_count;
static bool     g_dirty;

static int find(const char *key) {
    for (uint32_t i = 0; i < g_count; i++)
        if (strcmp(g_reg[i].key, key) == 0) return (int)i;
    return -1;
}

const char *reg_get(const char *key, const char *def) {
    int i = find(key);
    return i >= 0 ? g_reg[i].val : def;
}

int32_t reg_get_int(const char *key, int32_t def) {
    int i = find(key);
    if (i < 0) return def;
    char *end = nullptr;
    long v = strtol(g_reg[i].val, &end, 10);
    return (end && end != g_reg[i].val) ? (int32_t)v : def;
}

bool reg_set(const char *key, const char *value) {
    if (!key || !value) return false;
    if (strlen(key) >= REG_KEY_MAX || strlen(value) >= REG_VAL_MAX) return false;
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

bool reg_has(const char *key) { return find(key) >= 0; }
uint32_t reg_count(void) { return g_count; }
const char *reg_key_at(uint32_t i) { return i < g_count ? g_reg[i].key : nullptr; }

void reg_clear(void) { g_count = 0; g_dirty = true; }

void reg_load(const char *text, uint32_t len) {
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
        // reg_set enforces the length caps and dedups, so a malformed or
        // duplicated file cannot corrupt the table.
        reg_set(line, eq + 1);
    }
    g_dirty = false;   // a freshly loaded table matches the file on disk
}

uint32_t reg_serialize(char *buf, uint32_t cap) {
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
