#include "command.h"
#include "lock.h"
#include <string.h>

// A package loading on one core registers commands while the other resolves
// one. Read-mostly, but the write side moves entries during cmd_remove_owner.
static RpcLock  g_cmd_lock;
static Command  g_cmds[CMD_MAX];
static uint32_t g_count;

// Registrations that were refused — a full table, or a name already taken.
// cmd_register cannot report this usefully at its call site (the register
// functions are lists of struct literals), and a silently missing command is
// exactly the kind of bug that only shows up when someone types it. The shell
// checks this after boot and says so.
static uint32_t g_refused;

uint32_t cmd_refused(void) { return g_refused; }

bool cmd_register(const Command *cmd) {
    LockGuard _c(&g_cmd_lock);
    if (!cmd || !cmd->name || !cmd->fn)        { g_refused++; return false; }
    if (cmd_find(cmd->name))                   { g_refused++; return false; }  // no shadowing
    if (g_count >= CMD_MAX)                    { g_refused++; return false; }
    g_cmds[g_count++] = *cmd;
    return true;
}

void cmd_remove_owner(void *owner) {
    LockGuard _c(&g_cmd_lock);
    if (!owner) return;                      // never sweep the built-ins
    uint32_t w = 0;
    for (uint32_t r = 0; r < g_count; r++) {
        if (g_cmds[r].owner != owner) {
            if (w != r) g_cmds[w] = g_cmds[r];
            w++;
        }
    }
    g_count = w;
}

const Command *cmd_find(const char *name) {
    LockGuard _c(&g_cmd_lock);
    for (uint32_t i = 0; i < g_count; i++)
        if (strcmp(g_cmds[i].name, name) == 0) return &g_cmds[i];
    return nullptr;
}

uint32_t cmd_count(void) { return g_count; }

const Command *cmd_at(uint32_t i) { return i < g_count ? &g_cmds[i] : nullptr; }

// --- aliases ----------------------------------------------------------------

static Alias   g_aliases[ALIAS_MAX];
static uint32_t g_alias_count;

bool cmd_alias(const char *name, const char *target) {
    if (!name || !target) return false;
    if (g_alias_count >= ALIAS_MAX) { g_refused++; return false; }
    // A real command always wins its own name; an alias that shadowed one would
    // make `rm` mean something other than rm depending on registration order.
    if (cmd_find(name))         { g_refused++; return false; }
    if (cmd_alias_target(name)) { g_refused++; return false; }
    g_aliases[g_alias_count++] = {name, target};
    return true;
}

const char *cmd_alias_target(const char *name) {
    for (uint32_t i = 0; i < g_alias_count; i++)
        if (strcmp(g_aliases[i].name, name) == 0) return g_aliases[i].target;
    return nullptr;
}

uint32_t     cmd_alias_count(void)    { return g_alias_count; }
const Alias *cmd_alias_at(uint32_t i) { return i < g_alias_count ? &g_aliases[i] : nullptr; }

// --- user aliases -----------------------------------------------------------

struct UAlias { char name[UALIAS_NAME]; char value[UALIAS_VALUE]; };
static UAlias   g_ualias[UALIAS_MAX];
static uint32_t g_ualias_count;

static int ualias_find(const char *name) {
    for (uint32_t i = 0; i < g_ualias_count; i++)
        if (strcmp(g_ualias[i].name, name) == 0) return (int)i;
    return -1;
}

bool cmd_ualias_set(const char *name, const char *value) {
    if (!name || !value || !name[0] || !value[0]) return false;
    if (strlen(name) >= UALIAS_NAME || strlen(value) >= UALIAS_VALUE) return false;
    // A real command always keeps its own name. Letting an alias shadow `rm`
    // would make a destructive command mean something else without warning.
    if (cmd_find(name)) return false;

    int i = ualias_find(name);
    if (i < 0) {
        if (g_ualias_count >= UALIAS_MAX) return false;
        i = (int)g_ualias_count++;
        strcpy(g_ualias[i].name, name);
    }
    strcpy(g_ualias[i].value, value);
    return true;
}

bool cmd_ualias_remove(const char *name) {
    int i = ualias_find(name);
    if (i < 0) return false;
    for (uint32_t j = (uint32_t)i; j + 1 < g_ualias_count; j++) g_ualias[j] = g_ualias[j + 1];
    g_ualias_count--;
    return true;
}

const char *cmd_ualias_get(const char *name) {
    int i = ualias_find(name);
    return i < 0 ? nullptr : g_ualias[i].value;
}

uint32_t    cmd_ualias_count(void)         { return g_ualias_count; }
const char *cmd_ualias_name_at(uint32_t i) { return i < g_ualias_count ? g_ualias[i].name : nullptr; }

const Command *cmd_resolve(const char *name) {
    const Command *c = cmd_find(name);
    if (c) return c;
    const char *t = cmd_alias_target(name);
    return t ? cmd_find(t) : nullptr;
}
