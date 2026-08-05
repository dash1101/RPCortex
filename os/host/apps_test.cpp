// The package lifecycle: a resident app's commands go live on load and are
// swept when it unloads, and its image is freed. This exercises apps.cpp +
// command.cpp + the loader's app_unload together, with real malloc'd images so
// the free path actually runs.

#include "apps.h"
#include "command.h"
#include "loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// app_load isn't called here, but loader.cpp references api_lookup, so it must
// resolve at link time.
uint32_t api_lookup(const char *) { return 0; }
uint32_t api_symbol_count(void) { return 0; }
int api_index_of(const char *) { return -1; }
uint32_t api_addr_at(uint32_t) { return 0; }

// apps.cpp now also contains apps_launch, which references the kernel/storage/api
// seams. This test exercises only the resident TABLE (store/unload), never
// apps_launch, so these stubs exist purely to satisfy the linker.
uint32_t heap_free(void)  { return 1000; }
uint32_t heap_total(void) { return 2000; }
bool storage_open_source(const char *, AppSource *, void **) { return false; }
void storage_close_source(void *) {}
extern "C" void api_set_current_app(void *) {}
extern "C" volatile const char *g_current_app = nullptr;
// The fault handler's half of the contained-fault report lives in fault.cpp,
// which is device-only. Nothing here faults, so there is never one to read.
extern "C" int fault_report_contained(void) { return 0; }

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }
static int noop(int, char **) { return 0; }

// Build a fake resident app with real allocations, so apps_unload -> app_unload
// -> free() operates on genuine heap blocks.
static LoadedApp make_app(const char *name) {
    LoadedApp a;
    memset(&a, 0, sizeof(a));
    // Shaped like a real load: one block split into a read-only half and a
    // writable one, and the raw pointers recorded separately because those are
    // what app_unload gives back. A fixture that only filled in the aligned
    // pointers would leak on every unload and prove nothing about the real
    // path — which is what it did until app_unload started freeing the raw
    // ones, and ASan said so immediately.
    a.image_raw   = malloc(64);
    a.veneers_raw = malloc(16);
    a.image       = a.image_raw;
    a.veneers     = a.veneers_raw;
    a.image_size  = 64;
    a.text_size   = 32;
    a.data        = (char *)a.image + 32;
    a.data_size   = 32;
    a.veneer_size = 16;
    a.bytes_allocated = 80;
    strncpy(a.header.name, name, sizeof(a.header.name) - 1);
    return a;
}

// The argument vector a sandboxed package is given.
//
// argv points into the SHELL's memory, which a sandboxed package cannot read at
// all — so it is rebuilt inside the package's own stack before it runs. This
// checks the rebuilt copy says the same thing and lands entirely inside the
// block, because the two ways to get it wrong are a package acting on truncated
// arguments and a package writing over the stack it is about to use.
extern char **marshal_argv_for_test(void *base, uint32_t *top, int argc, char **argv);

static void check_argv(void) {
    static uint8_t stack[3072];
    char a0[] = "calc", a1[] = "2", a2[] = "^", a3[] = "3";
    char *argv[] = { a0, a1, a2, a3 };

    // Filled with something that is not zero, so the terminator has to be
    // WRITTEN rather than merely inherited from a blank page.
    memset(stack, 0xAA, sizeof(stack));
    uint32_t top = sizeof(stack);
    char **out = marshal_argv_for_test(stack, &top, 4, argv);
    ck(out != nullptr, "an ordinary argument vector is copied");
    if (out) {
        ck(strcmp(out[0], "calc") == 0 && strcmp(out[3], "3") == 0,
           "and says the same thing");
        ck(out[4] == nullptr, "with the vector terminated");
        // Everything must be INSIDE the package's stack. A copy that reached
        // outside it would be memory the package still cannot read, which is
        // the bug this exists to fix.
        bool inside = (uint8_t *)out >= stack && (uint8_t *)out < stack + sizeof(stack);
        for (int i = 0; i < 4; i++)
            if ((uint8_t *)out[i] < stack || (uint8_t *)out[i] >= stack + sizeof(stack))
                inside = false;
        ck(inside, "and every part of it is inside the package's own stack");
        // The stack top must move BELOW the copy, or the first push overwrites
        // the arguments the package is about to read.
        ck(top <= (uint32_t)((uint8_t *)out - stack),
           "and the stack now starts below it, not on top of it");
        ck((top & 7u) == 0, "eight-byte aligned, as a stack pointer must be");
    }

    // Arguments too big to fit are refused rather than truncated: a package
    // acting on half of what it was given is worse than one that did not run.
    static char big[4096];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    char *huge[] = { a0, big };
    top = sizeof(stack);
    ck(marshal_argv_for_test(stack, &top, 2, huge) == nullptr,
       "arguments that do not fit are refused, not cut short");
    ck(top == sizeof(stack), "and a refusal does not move the stack top");

    top = sizeof(stack);
    ck(marshal_argv_for_test(stack, &top, 0, nullptr) != nullptr,
       "no arguments at all is still a valid vector");
}

int main(void) {
    loader_set_allocator(malloc, free);

    LoadedApp a = make_app("greet");
    void *owner = a.image;

    // The package's command, tagged with its image as owner (what api.cpp does).
    Command c{"greet", "hi", noop, owner};
    ck(cmd_register(&c), "the package registers a command");
    ck(cmd_find("greet") != nullptr, "and it is live");

    ck(apps_store(&a) != nullptr, "the app becomes resident");
    LoadedApp dup = make_app("greet");
    ck(apps_store(&dup) == nullptr, "a second package of the same name is refused");
    free(dup.image); free(dup.veneers);   // the refused one is ours to clean up

    // Unload: the command must vanish and the image must be freed.
    ck(apps_unload("greet"), "the package unloads");
    ck(cmd_find("greet") == nullptr,
       "its command is swept — no dangling pointer into freed code");
    ck(!apps_unload("greet"), "unloading it again reports not-loaded");

    // A built-in (owner nullptr) registered alongside must survive a package
    // unload — cmd_remove_owner(nullptr behaviour) must never touch built-ins.
    Command bi{"help", "builtin", noop, nullptr};
    cmd_register(&bi);
    LoadedApp b = make_app("pkg2");
    Command c2{"p2cmd", "x", noop, b.image};
    cmd_register(&c2);
    apps_store(&b);
    ck(apps_unload("pkg2"), "second package unloads");
    ck(cmd_find("help") != nullptr, "the built-in survived the package unload");
    ck(cmd_find("p2cmd") == nullptr, "but the package command is gone");

    // Fill the table to its cap and confirm the next store is refused rather
    // than overrunning.
    int stored = 0;
    for (int i = 0; i < 32; i++) {
        char nm[16]; snprintf(nm, sizeof(nm), "f%d", i);
        LoadedApp f = make_app(nm);
        if (apps_store(&f)) stored++;
        else { free(f.image); free(f.veneers); }
    }
    ck(stored <= 16, "the resident table is capped, not unbounded");
    ck(stored == 16, "and holds exactly its capacity");

    check_argv();

    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
