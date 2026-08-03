#include "apps.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "kernel.h"
#include "blackbox.h"

#include <stdio.h>
#include <string.h>

// Set around app_main so a registered command is tagged with its app (api.cpp)
// and a fault names the culprit (fault.cpp).
extern "C" void api_set_current_app(void *owner);
extern "C" volatile const char *g_current_app;

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

int apps_launch(const char *file, int arg, bool quiet) {
    AppSource src;
    void *handle = nullptr;
    if (!storage_open_source(file, &src, &handle)) {
        if (!quiet) out_err("No such app: %s", file);
        return -1;
    }
    uint32_t before = heap_free();
    LoadedApp app;
    LoadResult rc = app_load(src, &app);
    storage_close_source(handle);
    if (rc != LOAD_OK) {
        if (!quiet) out_err("Load failed: %s%s%s", load_result_str(rc),
                            app.detail[0] ? " - " : "", app.detail);
        return -1;
    }

    api_set_current_app(app.image);
    g_current_app = app.header.name;
    // The first jump into loaded code. If a crash report stops here the loader
    // produced something that does not execute, which is a very different
    // problem from a package with a bug in it.
    bb_note_phase("entering app_main");
    int ret = app.entry(arg);
    bb_note_phase("app_main returned");
    g_current_app = nullptr;
    api_set_current_app(nullptr);

    // Resident iff it registered a command owned by its image.
    bool resident = false;
    for (uint32_t i = 0; i < cmd_count(); i++)
        if (cmd_at(i)->owner == app.image) { resident = true; break; }

    if (resident) {
        if (apps_store(&app)) {
            if (!quiet) out_ok("Package '%s' loaded.", app.header.name);
        } else {
            // Table full or already loaded: pull the commands back rather than
            // orphan them, and unload.
            cmd_remove_owner(app.image);
            app_unload(&app);
            if (!quiet) out_err("'%s' could not stay resident.", app.header.name);
        }
    } else {
        app_unload(&app);
        if (!quiet) {
            uint32_t after = heap_free();
            if (after == before) out_ok("'%s' finished  (exit %d).", app.header.name, ret);
            else out_warn("'%s' finished (exit %d) but did not release %u bytes.",
                          app.header.name, ret, (unsigned)(before - after));
        }
    }
    return ret;
}

static int cmd_apps(int, char **) {
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) continue;
        out_multi("  %s%-16s%s %u B", C_CYAN, g_apps[i].header.name, C_RESET,
                  (unsigned)g_apps[i].bytes_allocated);
        n++;
    }
    if (!n) out_multi("  (no packages loaded)");
    return 0;
}

static int cmd_unload(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: unload <package>"); return 1; }
    if (!apps_unload(argv[1])) { out_err("Not loaded: %s", argv[1]); return 1; }
    out_ok("Unloaded %s.", argv[1]);
    return 0;
}

void apps_register(void) {
    static const Command cmds[] = {
        {"apps",   "list resident packages", cmd_apps,   nullptr},
        {"unload", "unload a package",        cmd_unload, nullptr, LEVEL_ADMIN},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
