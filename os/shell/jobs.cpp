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
#include "lock.h"
#include "storage.h"
#include "interrupt.h"
#include "registry.h"
#include "logring.h"
#include "svcpolicy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_DIR      "/etc"
#define STARTUP_CFG  "/etc/startup.cfg"
#define TASKS_CFG    "/etc/tasks.cfg"
#define SERVICE_CFG  "/etc/services.cfg"
#define CFG_BUF      1024

int  shell_run_line_now(char *line);       // shell.cpp — runs one command line
int  shell_bg_spawn(const char *line);     // shell.cpp — `bg` that returns the pid

// --- the config files -------------------------------------------------------

static uint32_t cfg_read(const char *path, char *buf, uint32_t cap) {
    uint32_t n = storage_read_file(path, (uint8_t *)buf, cap - 1);
    buf[n] = 0;
    return n;
}

// A list file, read into a buffer that belongs to this call.
//
// These were five separate 1 KB static buffers, one per function. That was 5 KB
// of RAM held for the life of the device by code that runs at boot and when a
// list is edited — and being static, each was shared by every task that called
// it. `task add` on the shell and `service add` from a `bg` job or a scheduled
// line both went through list_command's one buffer, so either could write the
// other's list into its own file. Each call has its own now, from the heap, and
// gives it back on the way out.
struct CfgBuf {
    char    *p = nullptr;
    uint32_t len = 0;
    explicit CfgBuf(const char *path) {
        p = (char *)malloc(CFG_BUF);
        if (p) len = cfg_read(path, p, CFG_BUF);
        else   out_err("Not enough memory to read %s.", path);
    }
    ~CfgBuf() { free(p); }
    CfgBuf(const CfgBuf &) = delete;
    CfgBuf &operator=(const CfgBuf &) = delete;
};

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
    CfgBuf cfg(path);
    if (!cfg.p) return 1;
    char *buf = cfg.p;
    uint32_t len = cfg.len;

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
        uint32_t nl = joblist_add(buf, len, CFG_BUF, line);
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
    CfgBuf cfg(STARTUP_CFG);
    if (!cfg.p) return;
    const char *buf = cfg.p;
    uint32_t len = cfg.len;
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
// The table is rebuilt by `task add`/`remove`/`reload` on whichever task typed
// them, while the scheduler task reads it on another — possibly on the other
// core. Without this a reload could empty and refill the table under the
// scheduler as it copied a command out, and the line it ran could be half of one
// entry and half of another. Held for the rebuild and for the copy only, never
// while a command runs: a scheduled `task reload` takes it again.
static RpcLock  g_sched_lock;

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

// False when the list could not be read, and the schedule is left as it was.
static bool sched_reload(void) {
    LockGuard _lk(&g_sched_lock);
    CfgBuf cfg(TASKS_CFG);             // after the lock, so it is freed inside it
    if (!cfg.p) return false;
    g_nslots = 0;
    joblist_walk(cfg.p, cfg.len, load_slot, nullptr);
    return true;
}

static int sched_task(void *) {
    sched_reload();
    while (!task_should_stop()) {
        uint32_t now = task_now_ms() / 1000;
        for (uint32_t i = 0; ; i++) {
            char copy[160];
            {
                LockGuard _lk(&g_sched_lock);
                if (i >= g_nslots) break;
                if ((int32_t)(now - g_slots[i].next_at_s) < 0) continue;
                g_slots[i].next_at_s = now + g_slots[i].every_s;
                snprintf(copy, sizeof(copy), "%s", g_slots[i].cmd);
            }
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
        // It runs shell lines, so it gets the shell's stack (TASK_STACK_SHELL).
        g_sched_pid = task_spawn("scheduler", "(built-in)", sched_task, nullptr,
                                 TASK_STACK_SHELL, AFFINITY_ANY);
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
    if (argc >= 2 && !strcmp(argv[1], "reload")) {
        if (!sched_reload()) return 1;     // CfgBuf has said why
        out_ok("Schedule reloaded.");
        return 0;
    }

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
//
// Each service started is remembered with its pid, and a small task watches
// them: one that FAILS is started again, with a pause that doubles each time and
// a limit (core/svcpolicy.h has the rules and why). Before this a service that
// crashed stayed dead until the next reboot.
//
// The watcher only exists while there is something to watch, and wakes once a
// second, which is a pause measured in seconds needs no finer.

#define SVC_TRACK 8
struct Svc {
    bool     used;
    int      pid;
    SvcState st;
    char     line[128];
};
static Svc g_svcs[SVC_TRACK];
static int g_svc_watch_pid = -1;

// What the task behind a service looks like now. Read without a lock: these are
// whole words written by the task's own exit, and a torn read costs one second.
static SvcSeen svc_seen(int pid) {
    const TaskInfo *t = task_find(pid);
    if (!t || t->pid != pid) return SVC_GONE;
    if (t->state != TASK_DONE) return SVC_RUNNING;
    if (t->kill_requested) return SVC_KILLED;
    return t->exit_code == 0 ? SVC_FINISHED : SVC_FAILED;
}

static int svc_watch_task(void *) {
    while (!task_should_stop()) {
        uint32_t now = task_now_ms();
        for (int i = 0; i < SVC_TRACK; i++) {
            Svc &e = g_svcs[i];
            if (!e.used || e.st.retired) continue;
            switch (svc_decide(&e.st, svc_seen(e.pid), now)) {
            case SVC_RESTART: {
                // Started before the old one is cleared away, so a restart that
                // could not get a task leaves the failed one to be judged again
                // on the next pass, rather than losing track of it.
                int pid = shell_bg_spawn(e.line);
                if (pid < 0) break;
                task_reap(e.pid);
                e.pid = pid;
                svc_started(&e.st, now);
                log_addf(LOG_K_WARN, "service restarted after failing (%u of %u): %s",
                         (unsigned)e.st.failures, (unsigned)SVC_MAX_RESTARTS, e.line);
                break;
            }
            case SVC_GIVE_UP:
                log_addf(LOG_K_ERR, "service failed %u times in a row, not restarting: %s",
                         (unsigned)SVC_MAX_RESTARTS, e.line);
                break;
            case SVC_LET_GO:
                if (svc_seen(e.pid) == SVC_GONE)
                    log_addf(LOG_K_INFO, "service ended unseen, not restarted: %s", e.line);
                break;
            default:
                break;
            }
        }
        task_sleep_ms(1000);
    }
    return 0;
}

// Remember a service that has just been started, under its line, so the same
// line started again (pkg install puts back the services it stopped) replaces
// its entry rather than adding a second.
static void svc_track(const char *line, int pid) {
    Svc *e = nullptr;
    for (int i = 0; i < SVC_TRACK && !e; i++)
        if (g_svcs[i].used && !strcmp(g_svcs[i].line, line)) e = &g_svcs[i];
    for (int i = 0; i < SVC_TRACK && !e; i++)
        if (!g_svcs[i].used) e = &g_svcs[i];
    if (!e) return;                       // more services than watched slots: started, unwatched
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->pid  = pid;
    snprintf(e->line, sizeof(e->line), "%s", line);
    svc_started(&e->st, task_now_ms());

    if (g_svc_watch_pid > 0 && task_find(g_svc_watch_pid)) return;
    g_svc_watch_pid = task_spawn("svcwatch", "(built-in)", svc_watch_task, nullptr,
                                 TASK_STACK_DEF, AFFINITY_ANY);
}

// Start one service line and watch it. Returns the pid, or -1.
static int svc_start_line(const char *line) {
    int pid = shell_bg_spawn(line);
    if (pid < 0) {
        out_err("Could not start service: %s", line);
        return -1;
    }
    out_ok("[%d] %s", pid, line);
    svc_track(line, pid);
    return pid;
}

static void svc_start_cb(void *v, uint32_t, const char *line) {
    uint32_t *n = (uint32_t *)v;
    if (svc_start_line(line) > 0) (*n)++;
}

void jobs_start_services(void) {
    CfgBuf cfg(SERVICE_CFG);
    if (!cfg.p) return;
    const char *buf = cfg.p;
    uint32_t len = cfg.len;
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
    CfgBuf cfg(SERVICE_CFG);
    if (!cfg.p) return;
    joblist_walk(cfg.p, cfg.len, cb, ctx);
}

int jobs_service_start(const char *line) {
    if (!line || !line[0]) return 1;
    // Exactly what jobs_start_services does at boot, so a service restarted
    // here is indistinguishable from one that was never stopped - watched included.
    return svc_start_line(line) > 0 ? 0 : 1;
}

// What each watched service is doing, for `service status`.
static void svc_status(void) {
    bool any = false;
    for (int i = 0; i < SVC_TRACK; i++) {
        const Svc &e = g_svcs[i];
        if (!e.used) continue;
        any = true;
        const char *state;
        if (e.st.retired) {
            SvcSeen seen = svc_seen(e.pid);
            state = seen == SVC_FAILED ? "gave up" :
                    seen == SVC_KILLED ? "stopped" :
                    seen == SVC_FINISHED ? "finished" : "ended";
        } else {
            state = e.st.retry_at_ms ? "restarting" : "running";
        }
        out_multi("  %-10s pid %-4d %u restart%s  %s", state, e.pid,
                  (unsigned)e.st.failures, e.st.failures == 1 ? " " : "s", e.line);
    }
    if (!any) out_multi("  No services have been started.");
}

static int cmd_service(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "start")) { jobs_start_services(); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "status")) { svc_status(); return 0; }
    int rc = list_command(SERVICE_CFG, "Services", false, argc, argv,
                          "Usage: service [list | add <cmd> | remove <n> | clear | start | status]");
    if (argc < 2 || !strcmp(argv[1], "list"))
        out_multi("  %s'service status' shows each one live; a service that fails is restarted.%s",
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
