// startup, task, service, watch — the automation set.
//
// v1 had these too, but with one large caveat it could not remove: `task run`
// entered a FOREGROUND scheduler, because MicroPython had no way to fire a timer
// while the prompt was blocked on input. You got scheduled tasks or an
// interactive shell, not both, and the workaround was to boot straight into the
// scheduler and give up the prompt entirely.
//
// v2 has a real scheduler, so the timer is just another task. Scheduled work
// runs while you keep typing, which is the thing v1 wanted and could not have.
//
//   startup   commands run once, at boot
//   task      commands run again every N seconds, forever
//   service   long-running commands started at boot and supervised
//   watch     re-run something in the FOREGROUND until Ctrl+C
//
// All three lists are the same file-of-lines, handled by core/joblist.cpp.

#include "command.h"
#include "out.h"
#include "task.h"
#include "joblist.h"
#include "storage.h"
#include "interrupt.h"
#include "registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_DIR      "/etc"
#define STARTUP_CFG  "/etc/startup.cfg"
#define TASKS_CFG    "/etc/tasks.cfg"
#define SERVICE_CFG  "/etc/services.cfg"
#define CFG_BUF      1024

int  shell_run_line_now(char *line);       // shell.cpp — runs one command line

// --- the config files -------------------------------------------------------

static uint32_t cfg_read(const char *path, char *buf, uint32_t cap) {
    uint32_t n = storage_read_file(path, (uint8_t *)buf, cap - 1);
    buf[n] = 0;
    return n;
}

static bool cfg_write(const char *path, const char *buf, uint32_t len) {
    storage_mkdir(CFG_DIR);          // no-op when it already exists
    return storage_write_file(path, (const uint8_t *)buf, len);
}

struct ListCtx { const char *label; uint32_t shown; bool intervals; };

static void list_cb(void *v, uint32_t idx, const char *line) {
    ListCtx *c = (ListCtx *)v;
    c->shown++;
    if (c->intervals) {
        uint32_t secs; const char *cmd;
        if (joblist_split_interval(line, &secs, &cmd))
            out_multi("  %s%2u%s  every %s%4us%s  %s", C_CYAN, (unsigned)idx, C_RESET,
                      C_GRAY, (unsigned)secs, C_RESET, cmd);
        else
            out_multi("  %s%2u%s  %s(malformed)%s %s", C_CYAN, (unsigned)idx, C_RESET,
                      C_WARN, C_RESET, line);
        return;
    }
    out_multi("  %s%2u%s  %s", C_CYAN, (unsigned)idx, C_RESET, line);
}

// The shared body of startup/task/service: list, add, remove, clear.
static int list_command(const char *path, const char *label, bool intervals,
                        int argc, char **argv, const char *usage) {
    static char buf[CFG_BUF];
    uint32_t len = cfg_read(path, buf, sizeof(buf));

    const char *sub = (argc >= 2) ? argv[1] : "list";

    if (!strcmp(sub, "list")) {
        ListCtx c{label, 0, intervals};
        out_info("%s:", label);
        joblist_walk(buf, len, list_cb, &c);
        if (!c.shown) out_multi("  (none)");
        return 0;
    }

    if (!strcmp(sub, "add")) {
        if (argc < 3) { out_warn("%s", usage); return 1; }
        char line[160];
        line[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(line, " ", sizeof(line) - strlen(line) - 1);
            strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
        }
        if (intervals && !joblist_split_interval(line, nullptr, nullptr)) {
            out_err("Expected an interval first, e.g. 'task add 60 sysinfo'.");
            return 1;
        }
        uint32_t nl = joblist_add(buf, len, sizeof(buf), line);
        if (nl == len) { out_err("The list is full."); return 1; }
        if (!cfg_write(path, buf, nl)) { out_err("Could not write %s.", path); return 1; }
        out_ok("Added: %s", line);
        return 0;
    }

    if (!strcmp(sub, "remove") || !strcmp(sub, "rm") || !strcmp(sub, "del")) {
        if (argc < 3) { out_warn("Usage: %s remove <n>", argv[0]); return 1; }
        uint32_t idx = (uint32_t)strtoul(argv[2], nullptr, 10);
        char gone[160];
        if (!joblist_get(buf, len, idx, gone, sizeof(gone))) {
            out_err("No entry %u.", (unsigned)idx);
            return 1;
        }
        uint32_t nl = joblist_remove(buf, len, idx);
        if (!cfg_write(path, buf, nl)) { out_err("Could not write %s.", path); return 1; }
        out_ok("Removed: %s", gone);
        return 0;
    }

    if (!strcmp(sub, "clear")) {
        if (!cfg_write(path, "", 0)) { out_err("Could not write %s.", path); return 1; }
        out_ok("%s cleared.", label);
        return 0;
    }

    out_warn("%s", usage);
    return 1;
}

// --- startup ----------------------------------------------------------------

struct RunCtx { uint32_t ran; };

static void run_one_cb(void *v, uint32_t, const char *line) {
    RunCtx *c = (RunCtx *)v;
    char copy[160];
    snprintf(copy, sizeof(copy), "%s", line);
    shell_run_line_now(copy);
    c->ran++;
}

// Run the startup list. Called once after login, and by `startup run`.
void jobs_run_startup(void) {
    static char buf[CFG_BUF];
    uint32_t len = cfg_read(STARTUP_CFG, buf, sizeof(buf));
    if (!joblist_count(buf, len)) return;
    out_infop("Startup", "Running %u command%s...",
              (unsigned)joblist_count(buf, len),
              joblist_count(buf, len) == 1 ? "" : "s");
    RunCtx c{0};
    joblist_walk(buf, len, run_one_cb, &c);
}

static int cmd_startup(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "run")) { jobs_run_startup(); return 0; }
    return list_command(STARTUP_CFG, "Startup commands", false, argc, argv,
                        "Usage: startup [list | add <cmd> | remove <n> | clear | run]");
}

// --- scheduled tasks --------------------------------------------------------
//
// One background task holds the whole schedule rather than one task per entry:
// a dozen sleeping tasks would each cost a stack, and they would all be asleep
// almost all of the time. This wakes once a second, which is as fine-grained as
// an interval measured in seconds can be.

#define SCHED_MAX 8
struct Slot { uint32_t every_s; uint32_t next_at_s; char cmd[96]; };
static Slot     g_slots[SCHED_MAX];
static uint32_t g_nslots;
static int      g_sched_pid = -1;
static uint32_t g_fired;

static void load_slot(void *, uint32_t, const char *line) {
    if (g_nslots >= SCHED_MAX) return;
    uint32_t secs; const char *cmd;
    if (!joblist_split_interval(line, &secs, &cmd)) return;
    if (secs == 0) return;                     // would be a busy loop, not a task
    Slot &s = g_slots[g_nslots++];
    s.every_s   = secs;
    // First run is one full interval away, not immediately — `task add 3600 x`
    // should not fire the moment it is added.
    s.next_at_s = (task_now_ms() / 1000) + secs;
    snprintf(s.cmd, sizeof(s.cmd), "%s", cmd);
}

static void sched_reload(void) {
    static char buf[CFG_BUF];
    uint32_t len = cfg_read(TASKS_CFG, buf, sizeof(buf));
    g_nslots = 0;
    joblist_walk(buf, len, load_slot, nullptr);
}

static int sched_task(void *) {
    sched_reload();
    while (!task_should_stop()) {
        uint32_t now = task_now_ms() / 1000;
        for (uint32_t i = 0; i < g_nslots; i++) {
            if ((int32_t)(now - g_slots[i].next_at_s) < 0) continue;
            g_slots[i].next_at_s = now + g_slots[i].every_s;
            char copy[160];
            snprintf(copy, sizeof(copy), "%s", g_slots[i].cmd);
            shell_run_line_now(copy);
            g_fired++;
        }
        task_sleep_ms(1000);
    }
    return 0;
}

uint32_t jobs_fired(void) { return g_fired; }

static int cmd_task(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        if (g_sched_pid > 0 && task_find(g_sched_pid)) {
            out_warn("The scheduler is already running (pid %d).", g_sched_pid);
            return 1;
        }
        g_sched_pid = task_spawn("scheduler", "(built-in)", sched_task, nullptr,
                                 TASK_STACK_DEF, AFFINITY_ANY);
        if (g_sched_pid < 0) { out_err("Could not start the scheduler."); return 1; }
        out_ok("Scheduler running as pid %d.", g_sched_pid);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "stop")) {
        if (g_sched_pid < 0 || !task_kill(g_sched_pid)) {
            out_warn("The scheduler is not running.");
            return 1;
        }
        out_ok("Scheduler stopping.");
        g_sched_pid = -1;
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "reload")) { sched_reload(); out_ok("Schedule reloaded."); return 0; }

    int rc = list_command(TASKS_CFG, "Scheduled tasks", true, argc, argv,
                          "Usage: task [list | add <secs> <cmd> | remove <n> | clear"
                          " | start | stop | reload]");
    // An edit that does not reach the running scheduler is a trap: the list says
    // one thing and the device does another.
    if (rc == 0 && argc >= 2 && strcmp(argv[1], "list") != 0) sched_reload();

    if (argc < 2 || !strcmp(argv[1], "list")) {
        bool live = (g_sched_pid > 0 && task_find(g_sched_pid));
        out_multi("  %sScheduler: %s%s", C_GRAY,
                  live ? "running" : "stopped — 'task start' to run them", C_RESET);
    }
    return rc;
}

// --- services ---------------------------------------------------------------

static void svc_start_cb(void *v, uint32_t, const char *line) {
    uint32_t *n = (uint32_t *)v;
    char copy[160];
    snprintf(copy, sizeof(copy), "bg %s", line);
    shell_run_line_now(copy);
    (*n)++;
}

void jobs_start_services(void) {
    static char buf[CFG_BUF];
    uint32_t len = cfg_read(SERVICE_CFG, buf, sizeof(buf));
    if (!joblist_count(buf, len)) return;
    uint32_t n = 0;
    joblist_walk(buf, len, svc_start_cb, &n);
    out_okp("Services", "Started %u.", (unsigned)n);
}

// Read the service list, and start one entry from it.
//
// Both exist for `pkg install`, which has to work out whether the package it is
// replacing is busy because a SERVICE is running it — and, if so, put that
// service back afterwards. The path to the file stays here, where the rest of
// the list lives, rather than being spelt out a second time somewhere else.
//
// jobs_service_start takes a whole line rather than an index, because between
// the stop and the restart an install has rewritten the package and could in
// principle have rewritten the list too. The line the caller stopped is the
// line it puts back.
void jobs_services_walk(JobWalkFn cb, void *ctx) {
    static char buf[CFG_BUF];
    uint32_t len = cfg_read(SERVICE_CFG, buf, sizeof(buf));
    joblist_walk(buf, len, cb, ctx);
}

int jobs_service_start(const char *line) {
    if (!line || !line[0]) return 1;
    char copy[160];
    // Exactly what jobs_start_services does at boot, so a service restarted
    // here is indistinguishable from one that was never stopped.
    snprintf(copy, sizeof(copy), "bg %s", line);
    return shell_run_line_now(copy);
}

static int cmd_service(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "start")) { jobs_start_services(); return 0; }
    int rc = list_command(SERVICE_CFG, "Services", false, argc, argv,
                          "Usage: service [list | add <cmd> | remove <n> | clear | start]");
    if (argc < 2 || !strcmp(argv[1], "list"))
        out_multi("  %sLive state is in 'ps'; services run as ordinary tasks.%s",
                  C_GRAY, C_RESET);
    return rc;
}

// --- watch ------------------------------------------------------------------

static int cmd_watch(int argc, char **argv) {
    if (argc < 2) { out_warn("Usage: watch [-n <secs>] <command>"); return 1; }

    uint32_t interval = 2;
    int first = 1;
    if (!strcmp(argv[1], "-n")) {
        if (argc < 4) { out_warn("Usage: watch -n <secs> <command>"); return 1; }
        char *end = nullptr;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end || v == 0) { out_warn("Invalid interval: '%s'", argv[2]); return 1; }
        interval = (uint32_t)v;
        first = 3;
    }

    char line[160];
    line[0] = 0;
    for (int i = first; i < argc; i++) {
        if (i > first) strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }
    if (!line[0]) { out_warn("No command to watch."); return 1; }

    intr_clear();
    uint32_t round = 0;
    while (!intr_check()) {
        printf("\033[2J\033[H");        // clear and home, so it redraws in place
        out_multi("%sEvery %us:  %s%s%s      (Ctrl+C to stop, pass %u)%s",
                  C_GRAY, (unsigned)interval, C_RESET, line, C_GRAY,
                  (unsigned)++round, C_RESET);
        out_blank();

        char copy[160];
        snprintf(copy, sizeof(copy), "%s", line);
        shell_run_line_now(copy);

        // Sliced so Ctrl+C lands during the gap, not only while the command
        // itself is running.
        for (uint32_t i = 0; i < interval * 10 && !intr_check(); i++)
            task_sleep_ms(100);
    }
    out_blank();
    out_info("watch stopped.");
    return 0;
}

void jobs_register(void) {
    static const Command cmds[] = {
        {"startup", "commands run at boot",            cmd_startup, nullptr, LEVEL_ADMIN},
        {"task",    "commands run on a timer",         cmd_task,    nullptr, LEVEL_ADMIN},
        {"service", "long-running background commands", cmd_service, nullptr, LEVEL_ADMIN},
        {"watch",   "watch [-n secs] <command>",       cmd_watch,   nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
