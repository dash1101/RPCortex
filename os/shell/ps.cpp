// The task manager — ps, kill, jobs.
//
// v1 never had this and it was missed: when a background service misbehaved
// there was no way to see it, let alone stop it. Everything that runs here is a
// task with a pid, a name, a path, a stack and a CPU figure, so it can all be
// listed and all be stopped.

#include "command.h"
#include "out.h"
#include "task.h"
#include "fmt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *state_text(TaskState s) {
    switch (s) {
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "run";
        case TASK_SLEEPING: return "sleep";
        case TASK_BLOCKED:  return "block";
        case TASK_DONE:     return "done";
        default:            return "-";
    }
}

static const char *state_colour(TaskState s) {
    switch (s) {
        case TASK_RUNNING:  return C_CYAN;
        case TASK_DONE:     return C_GRAY;
        case TASK_BLOCKED:  return C_WARN;
        default:            return C_RESET;
    }
}

static int cmd_ps(int argc, char **argv) {
    bool wide = (argc >= 2 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "--long")));

    out_multi("  %-4s %-12s %-6s %-4s %-11s %-8s %s",
              "PID", "NAME", "STATE", "CORE", "STACK", "CPU", wide ? "PATH" : "");
    out_multi("  %s------------------------------------------------------------%s",
              C_GRAY, C_RESET);

    uint32_t shown = 0;
    for (uint32_t i = 0; i < task_count(); i++) {
        const TaskInfo *t = task_at(i);
        if (!t) break;
        shown++;

        // Stack as used/total, which is the number that predicts an overflow.
        // A task with no measurable stack is the adopted boot context, whose
        // stack belongs to the C runtime rather than to the scheduler.
        char stack[16];
        if (t->stack_size) {
            char used[8], total[8];
            fmt_size(t->stack_used, used, sizeof(used));
            fmt_size(t->stack_size, total, sizeof(total));
            snprintf(stack, sizeof(stack), "%s/%s", used, total);
        } else {
            snprintf(stack, sizeof(stack), "-");
        }

        char cpu[12];
        if (t->cpu_ms >= 1000) snprintf(cpu, sizeof(cpu), "%u.%us",
                                        (unsigned)(t->cpu_ms / 1000),
                                        (unsigned)((t->cpu_ms % 1000) / 100));
        else                   snprintf(cpu, sizeof(cpu), "%ums", (unsigned)t->cpu_ms);

        out_multi("  %s%-4d%s %-12s %s%-6s%s %-4u %-11s %-8s %s",
                  C_CYAN, t->pid, C_RESET,
                  t->name,
                  state_colour(t->state), state_text(t->state), C_RESET,
                  (unsigned)t->core,
                  stack, cpu,
                  wide ? t->path : "");

        // A stack over 80% used is about to be a memory-corruption bug rather
        // than a crash, so it is worth saying before it happens.
        if (t->stack_size && t->stack_used * 100 / t->stack_size >= 80)
            out_warn("  pid %d is using %u%% of its stack.", t->pid,
                     (unsigned)(t->stack_used * 100 / t->stack_size));
    }

    out_multi("  %s%u task%s%s", C_GRAY, (unsigned)shown, shown == 1 ? "" : "s", C_RESET);
    return 0;
}

static int cmd_kill(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: kill <pid> [pid...]"); return 1; }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char *end = nullptr;
        long pid = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end) { out_err("Not a pid: %s", argv[i]); bad++; continue; }

        const TaskInfo *t = task_find((int)pid);
        if (!t) { out_err("No task with pid %ld.", pid); bad++; continue; }

        // A finished task is reaped rather than killed — same command, since
        // "get rid of this line in ps" is the same intent either way.
        if (t->state == TASK_DONE) {
            task_reap((int)pid);
            out_ok("Cleared finished task %ld.", pid);
            continue;
        }
        if (!task_kill((int)pid)) {
            out_err("Cannot stop pid %ld.%s", pid, pid == 1 ? "  It is the shell." : "");
            bad++;
            continue;
        }
        // Cooperative: it stops at its next yield point, where it is not holding
        // a lock or part-way through a flash write. Saying so is better than
        // implying it is already gone.
        out_ok("Asked '%s' (pid %ld) to stop.", t->name, pid);
    }
    return bad ? 1 : 0;
}

void ps_register(void) {
    static const Command cmds[] = {
        {"ps",   "list running tasks  (-l for paths)", cmd_ps,   nullptr},
        {"kill", "kill <pid>",                         cmd_kill, nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
    cmd_alias("tasks", "ps");
    cmd_alias("jobs",  "ps");
}
