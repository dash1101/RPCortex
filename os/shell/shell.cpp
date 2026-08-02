// The shell: a prompt, a line parser, and the built-in commands.
//
// The v1 launchpad in miniature and without the interpreter machinery — no
// _get_scope, no .mpy resolution, no _cmd_cache, none of the apparatus that
// existed to manage Python modules. A line is split into argv, the first token
// is looked up in the registry, and the command runs. That is the whole engine.

#include "shell.h"
#include "command.h"
#include "kernel.h"
#include "loader.h"
#include "storage.h"
#include "session.h"
#include "registry.h"
#include "users.h"
#include "persist.h"
#include "apps.h"
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

// Set by the shell around app_main so a registered command is tagged with its
// app (api.cpp), and so a fault names the culprit (fault.cpp).
extern "C" void api_set_current_app(void *owner);
extern "C" volatile const char *g_current_app;

// The filesystem commands live in fs.cpp and carry the cwd.
void fs_register(void);
const char *fs_cwd(void);
void text_register(void);
void sys_register(void);

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

static int split_args(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

// --- built-in commands ------------------------------------------------------

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("commands (%u):\n", (unsigned)cmd_count());
    for (uint32_t i = 0; i < cmd_count(); i++) {
        const Command *c = cmd_at(i);
        printf("  %-10s %s%s\n", c->name, c->help,
               c->owner ? "  [app]" : "");
    }
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("RPCortex %s  (C++)  board %s\n", RPC_OS_VERSION, PICO_BOARD);
    return 0;
}

static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("  heap : %u / %u KB free\n",
           (unsigned)(heap_free() / 1024), (unsigned)(heap_total() / 1024));
    printf("  disk : %u KB free\n", (unsigned)(storage_free_bytes() / 1024));
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    storage_list();
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\x1b[2J\x1b[H");
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("rebooting\n");
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
    while (1) {}
    return 0;
}

static int cmd_put(int argc, char **argv) {
    if (argc < 3) { printf("usage: put <name> <len>\n"); return 1; }
    uint32_t len = (uint32_t)strtoul(argv[2], nullptr, 10);
    if (len == 0 || len > 256 * 1024) { printf("bad length\n"); return 1; }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { printf("out of memory\n"); return 1; }
    printf("send %u raw bytes\n", (unsigned)len);
    for (uint32_t i = 0; i < len; i++) {
        int c;
        do { c = getchar_timeout_us(5000000); } while (c == PICO_ERROR_TIMEOUT);
        buf[i] = (uint8_t)c;
    }
    bool ok = storage_write_file(argv[1], buf, len);
    free(buf);
    printf("%s %s (%u B)\n", ok ? "wrote" : "FAILED", argv[1], (unsigned)len);
    return ok ? 0 : 1;
}

// run — the load-run-resident flow now lives in apps_launch, shared with
// `pkg install` and boot-time package loading, so there is one code path.
static int cmd_run(int argc, char **argv) {
    if (argc < 2) { printf("usage: run <app> [arg]\n"); return 1; }
    int arg = (argc >= 3) ? (int)strtol(argv[2], nullptr, 10) : 0;
    return apps_launch(argv[1], arg, /*quiet*/false) < 0 ? 1 : 0;
}

static int cmd_whoami(int, char **) {
    printf("%s%s\n", session_user(), users_is_admin(session_user()) ? " (admin)" : "");
    return 0;
}

static int cmd_users(int, char **) {
    for (uint32_t i = 0; i < users_count(); i++) {
        const char *n = users_name_at(i);
        printf("  %-16s%s%s\n", n,
               users_is_admin(n)  ? " admin"  : "",
               users_is_nopass(n) ? " nopass" : "");
    }
    return 0;
}

static int cmd_reg(int argc, char **argv) {
    if (argc == 1) {
        for (uint32_t i = 0; i < reg_count(); i++) {
            const char *k = reg_key_at(i);
            printf("  %s=%s\n", k, reg_get(k, ""));
        }
        return 0;
    }
    if (!strcmp(argv[1], "get") && argc >= 3) {
        printf("%s\n", reg_get(argv[2], "(unset)"));
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc >= 4) {
        if (!reg_set(argv[2], argv[3])) { printf("could not set\n"); return 1; }
        return 0;   // written to flash by the shell's post-command save
    }
    printf("usage: reg | reg get <key> | reg set <key> <value>\n");
    return 1;
}

static int cmd_logout(int, char **) {
    printf("logging out\n");
    session_logout();
    session_boot();      // back to the login prompt
    return 0;
}

void shell_register_builtins(void) {
    static const Command builtins[] = {
        {"help",   "list commands",                 cmd_help,   nullptr},
        {"ver",    "version and board",             cmd_ver,    nullptr},
        {"mem",    "heap and disk free",            cmd_mem,    nullptr},
        {"ls",     "list stored apps",              cmd_ls,     nullptr},
        {"run",    "run <app> [arg]",               cmd_run,    nullptr},
        {"put",    "put <name> <len>  upload bytes", cmd_put,   nullptr},
        {"clear",  "clear the screen",              cmd_clear,  nullptr},
        {"reboot", "restart the device",            cmd_reboot, nullptr},
        {"whoami", "the logged-in user",            cmd_whoami, nullptr},
        {"users",  "list accounts",                 cmd_users,  nullptr},
        {"reg",    "reg | reg get K | reg set K V", cmd_reg,    nullptr},
        {"logout", "return to the login prompt",    cmd_logout, nullptr},
    };
    for (const auto &c : builtins) cmd_register(&c);
    fs_register();          // cd/ls/cat/... register alongside
    apps_register();        // apps / unload for resident packages
    pkg_init();             // ensure /pkg exists
    pkg_register();         // install / remove / list
    text_register();        // echo / grep / wc / head / tail / find
    sys_register();         // uptime / date / sysinfo
}

void shell_run(void) {
    char line[128];
    char *argv[16];
    printf("\ntype 'help'\n");
    while (true) {
        printf("%s:%s> ", session_user(), fs_cwd());
        if (!read_line(line, sizeof(line))) continue;
        int argc = split_args(line, argv, 16);
        if (argc == 0) continue;
        const Command *c = cmd_find(argv[0]);
        if (!c) { printf("unknown: %s\n", argv[0]); continue; }
        c->fn(argc, argv);
        // Persist registry/users only if a command actually changed them — the
        // dirty flags guard the flash write, so this is free when nothing moved.
        persist_save_dirty();
    }
}
