// RPCortex v2.0 loader spike — firmware entry point.
//
// A prompt over USB serial with just enough commands to demonstrate the loader
// and produce the numbers the spike exists to produce. This is NOT the start of
// the v2.0 shell: no command table, no filesystem commands, no networking.
// Everything here exists to exercise app_load and report what it cost.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"

#include "loader.h"
#include "storage.h"
#include "api.h"

extern "C" volatile const char *g_current_app;

// --- heap accounting --------------------------------------------------------
// newlib exposes the break; comparing it against the stack gives the free gap.
// Crude, but it is the number that matters: whether loading an app leaks.
extern "C" {
extern char __StackLimit;
extern char __bss_end__;
}

static uint32_t heap_free(void) {
    // Total heap arena minus what malloc currently has handed out. newlib's
    // mallinfo is the only honest source here: the break alone would ignore
    // freed blocks that were never returned to the OS.
    struct mallinfo mi = mallinfo();
    uint32_t total = (uint32_t)(&__StackLimit - &__bss_end__);
    return total - (uint32_t)mi.uordblks;
}

// --- line input -------------------------------------------------------------
static bool read_line(char *buf, size_t max) {
    size_t n = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) { sleep_ms(2); continue; }
        if (c == '\r' || c == '\n') { putchar('\n'); buf[n] = 0; return true; }
        if ((c == 8 || c == 127) && n) { n--; printf("\b \b"); continue; }
        if (c >= 32 && c < 127 && n + 1 < max) { buf[n++] = (char)c; putchar(c); }
    }
}

// --- commands ---------------------------------------------------------------

static void cmd_run(const char *name, int arg) {
    AppSource src;
    void *handle = nullptr;
    if (!storage_open_source(name, &src, &handle)) {
        printf("  no such app: %s\n", name);
        return;
    }

    uint32_t before = heap_free();
    LoadedApp app;
    LoadResult rc = app_load(src, &app);
    storage_close_source(handle);

    if (rc != LOAD_OK) {
        printf("  load failed: %s", load_result_str(rc));
        if (app.detail[0]) printf(" (%s)", app.detail);
        printf("\n");
        return;
    }

    uint32_t after_load = heap_free();
    printf("  loaded '%s' api=%u.%u  image=%u B  veneers=%u B (%u used)\n",
           app.header.name, app.header.api_major, app.header.api_minor,
           app.image_size, app.veneer_size, app.veneers_used);
    printf("  heap: %u -> %u  (app cost %u B)\n",
           before, after_load, before - after_load);

    printf("  --- app output ---\n");
    g_current_app = app.header.name;
    int ret = app.entry(arg);
    g_current_app = nullptr;
    printf("  --- returned %d ---\n", ret);

    app_unload(&app);
    uint32_t after_unload = heap_free();
    printf("  heap after unload: %u  (%s)\n", after_unload,
           after_unload == before ? "fully reclaimed"
                                  : "LEAKED -- investigate");
}

static void cmd_put(const char *name, uint32_t len) {
    if (len == 0 || len > 128 * 1024) { printf("  bad length\n"); return; }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { printf("  out of memory for %u B\n", len); return; }
    printf("  send %u raw bytes now\n", len);
    for (uint32_t i = 0; i < len; i++) {
        int c;
        do { c = getchar_timeout_us(5 * 1000 * 1000); } while (c == PICO_ERROR_TIMEOUT);
        buf[i] = (uint8_t)c;
    }
    bool ok = storage_write_file(name, buf, len);
    free(buf);
    printf("  %s %s (%u B)\n", ok ? "wrote" : "FAILED to write", name, len);
}

static void banner(void) {
    printf("\n");
    printf("RPCortex v2.0 loader spike\n");
    printf("  target      : %s\n", PICO_BOARD);
    printf("  app ABI     : %u.%u  (%u exported symbols)\n",
           RPC_API_MAJOR, RPC_API_MINOR, (unsigned)api_symbol_count());
    printf("  flash       : %u KB\n", PICO_FLASH_SIZE_BYTES / 1024);
    printf("  heap free   : %u B\n", heap_free());
    printf("  fs free     : %u B\n", storage_free_bytes());
    printf("\n  commands: ls | put <name> <len> | run <name> [arg] | mem | help\n\n");
}

int main(void) {
    stdio_init_all();
    // Wait for a terminal, but not forever — a headless boot must still run.
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(10);
    sleep_ms(200);

    if (!storage_init(true)) printf("  storage init FAILED\n");
    banner();

    char line[96];
    while (true) {
        printf("v2> ");
        if (!read_line(line, sizeof(line))) continue;

        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        if (!strcmp(cmd, "ls")) {
            storage_list();
        } else if (!strcmp(cmd, "mem")) {
            printf("  heap free : %u B\n", heap_free());
            printf("  fs free   : %u B\n", storage_free_bytes());
        } else if (!strcmp(cmd, "put")) {
            char *nm = strtok(nullptr, " ");
            char *ln = strtok(nullptr, " ");
            if (!nm || !ln) printf("  usage: put <name> <len>\n");
            else cmd_put(nm, (uint32_t)strtoul(ln, nullptr, 10));
        } else if (!strcmp(cmd, "run")) {
            char *nm = strtok(nullptr, " ");
            char *ar = strtok(nullptr, " ");
            if (!nm) printf("  usage: run <name> [arg]\n");
            else cmd_run(nm, ar ? (int)strtol(ar, nullptr, 10) : 1);
        } else if (!strcmp(cmd, "help")) {
            banner();
        } else {
            printf("  unknown: %s\n", cmd);
        }
    }
}
