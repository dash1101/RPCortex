#include "command.h"
#include <string.h>

static Command  g_cmds[CMD_MAX];
static uint32_t g_count;

bool cmd_register(const Command *cmd) {
    if (!cmd || !cmd->name || !cmd->fn) return false;
    if (cmd_find(cmd->name)) return false;   // no shadowing an existing command
    if (g_count >= CMD_MAX) return false;
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
