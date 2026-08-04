#include "apps.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "kernel.h"
#include "blackbox.h"
#include "mpu.h"

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

// --- entering package code --------------------------------------------------

static void describe(const LoadedApp *a, TaskAppMem *m) {
    m->text        = a->image;
    m->text_size   = a->text_size;
    m->data        = a->data;
    m->data_size   = a->data_size;
    m->veneer      = a->veneers;
    // Only the part actually written, rounded up to a whole block.
    //
    // The pool is sized for the worst case — one veneer per relocation — and a
    // typical package uses a third of it, so covering the whole allocation
    // would put a read-only region over bytes that are still ordinary free
    // heap. But a veneer is SIXTEEN bytes and a region is described in
    // thirty-twos, so the used length is a multiple of 32 only half the time,
    // and the other half the hardware would refuse the region outright — which
    // reads as "the veneers are simply not protected" and says nothing at all.
    // Rounding up stays inside the pool, because the allocation itself was
    // padded to a whole number of blocks.
    m->veneer_size = mpu_align_up(a->veneers_used, MPU_V8_GRAIN);
    if (m->veneer_size > a->veneer_size) m->veneer_size = a->veneer_size;
}

void app_enter(const LoadedApp *app, TaskAppMem *saved, bool *had_saved) {
    *had_saved = task_app_mem_get(saved);
    if (!app) return;
    TaskAppMem m;
    describe(app, &m);
    task_app_mem_set(&m);
}

void app_leave(const TaskAppMem *saved, bool had_saved) {
    if (had_saved) task_app_mem_set(saved);
    else           task_app_mem_clear();
}

bool app_enter_owner(const void *owner, TaskAppMem *saved, bool *had_saved) {
    *had_saved = false;
    if (!owner) return false;                  // a built-in: nothing to protect
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i] || g_apps[i].image != owner) continue;
        app_enter(&g_apps[i], saved, had_saved);
        return true;
    }
    return false;
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

    // A package that registers nothing usually just finished its work. One that
    // TRIED and was refused is a different thing entirely, and used to be
    // invisible: pkg reported it installed, the command did not exist, and
    // nothing anywhere connected the two. The refusal counter is what tells
    // them apart.
    uint32_t refused_before = cmd_refused();

    api_set_current_app(app.image);
    g_current_app = app.header.name;
    // The first jump into loaded code. If a crash report stops here the loader
    // produced something that does not execute, which is a very different
    // problem from a package with a bug in it.
    //
    // The protection goes on immediately before that jump and comes off
    // immediately after, and not one line wider. Outside this window the image
    // is ordinary heap that the loader wrote and app_unload is about to hand
    // back — and free() writes its own bookkeeping into a block it is
    // reclaiming, which a read-only region would fault on.
    bb_note_phase("entering app_main");
    TaskAppMem saved;
    bool had_saved = false;
    app_enter(&app, &saved, &had_saved);
    int ret = app.entry(arg);
    app_leave(&saved, had_saved);
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
    } else if (cmd_refused() != refused_before) {
        // It asked for a command and did not get one. Almost always the table
        // being full, which is a limit rather than a mistake in the package.
        app_unload(&app);
        out_errp("apps", "'%s' could not register its command%s.",
                 app.header.name,
                 cmd_refused() - refused_before > 1 ? "s" : "");
        out_multi("  The command table is full (%d), or the name is taken.", CMD_MAX);
        return -1;
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

// Find the package a raw address belongs to. A fault report giving only an
// absolute SRAM address is nearly useless — the image lands wherever malloc put
// it, so the number differs every boot. An offset into a named package can be
// looked up directly in the .app.
extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer) {
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) continue;
        uint32_t base = (uint32_t)(uintptr_t)g_apps[i].image;
        if (addr >= base && addr < base + g_apps[i].image_size) {
            if (offset)    *offset = addr - base;
            if (in_veneer) *in_veneer = false;
            return g_apps[i].header.name;
        }
        uint32_t vbase = (uint32_t)(uintptr_t)g_apps[i].veneers;
        if (addr >= vbase && addr < vbase + g_apps[i].veneer_size) {
            if (offset)    *offset = addr - vbase;
            if (in_veneer) *in_veneer = true;
            return g_apps[i].header.name;
        }
    }
    return nullptr;
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
