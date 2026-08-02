#include "command.h"
#include <string.h>

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
    if (!cmd || !cmd->name || !cmd->fn)        { g_refused++; return false; }
    if (cmd_find(cmd->name))                   { g_refused++; return false; }  // no shadowing
    if (g_count >= CMD_MAX)                    { g_refused++; return false; }
    g_cmds[g_count++] = *cmd;
    return true;
}

void cmd_remove_owner(void *owner) {
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

const Command *cmd_resolve(const char *name) {
    const Command *c = cmd_find(name);
    if (c) return c;
    const char *t = cmd_alias_target(name);
    return t ? cmd_find(t) : nullptr;
}
