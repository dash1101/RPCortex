#include "apps.h"
#include "command.h"

#include <stdio.h>
#include <string.h>

// Fixed table, no allocation for the bookkeeping itself. A device that somehow
// loads 16 resident packages has a bigger problem than a full table.
#define APPS_MAX 16

static LoadedApp g_apps[APPS_MAX];
static bool      g_used[APPS_MAX];

static LoadedApp *find(const char *name) {
    for (int i = 0; i < APPS_MAX; i++)
        if (g_used[i] && strcmp(g_apps[i].header.name, name) == 0) return &g_apps[i];
    return nullptr;
}

LoadedApp *apps_store(const LoadedApp *app) {
    if (find(app->header.name)) return nullptr;      // already resident
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) {
            g_apps[i] = *app;      // image/veneer pointers copy over and stay valid
            g_used[i] = true;
            return &g_apps[i];
        }
    }
    return nullptr;
}

bool apps_unload(const char *name) {
    LoadedApp *a = find(name);
    if (!a) return false;
    // Order matters: drop the commands FIRST (while the code they point into is
    // still mapped), then free the image. The reverse would leave the registry
    // holding function pointers into freed memory for the moment between.
    cmd_remove_owner(a->image);
    app_unload(a);
    for (int i = 0; i < APPS_MAX; i++) if (&g_apps[i] == a) { g_used[i] = false; break; }
    return true;
}

static int cmd_apps(int, char **) {
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) continue;
        printf("  %-16s %u B\n", g_apps[i].header.name,
               (unsigned)g_apps[i].bytes_allocated);
        n++;
    }
    if (!n) printf("  (no resident packages)\n");
    return 0;
}

static int cmd_unload(int argc, char **argv) {
    if (argc < 2) { printf("usage: unload <package>\n"); return 1; }
    if (!apps_unload(argv[1])) { printf("not loaded: %s\n", argv[1]); return 1; }
    printf("unloaded %s\n", argv[1]);
    return 0;
}

void apps_register(void) {
    static const Command cmds[] = {
        {"apps",   "list resident packages", cmd_apps,   nullptr},
        {"unload", "unload a package",        cmd_unload, nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
