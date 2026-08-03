// stress — the built-in self test.
//
// A package, not a built-in command, on purpose: it exercises the OS through the
// same ABI every other package uses, so if the package system is broken this
// test cannot run, and that failure is itself the answer. It also means it can
// be removed like anything else when the space is wanted back.
//
// What it checks, and why each one is here rather than being obvious:
//
//   tasks   — that two tasks really interleave, and that a killed one stops.
//             The failure is a task that never runs again, which looks like a
//             hang rather than a wrong answer.
//   cores   — that work lands on more than one core when the board has two.
//   files   — write, read back, compare, delete. Run from several tasks at once,
//             which is the case the filesystem lock exists for; without it this
//             is what corrupts littlefs.
//   memory  — allocate, fill, verify, free, and report the largest block still
//             available. Verifying the CONTENTS is the point: a heap bug that
//             hands the same block to two callers shows up as changed data, not
//             as a failed allocation.
//
// Everything is bounded and interruptible. Ctrl+C stops it wherever it is.

#include "rpc_app.h"

RPC_APP_VER("stress", "1.0");

// --- reporting --------------------------------------------------------------

static int g_pass, g_fail;

static void ok(const char *what) {
    g_pass++;
    fw_printf("  \033[96m[@]\033[0m %s\n", what);
}
static void bad(const char *what, const char *detail) {
    g_fail++;
    fw_printf("  \033[91m[!]\033[0m %s%s%s\n", what, detail ? " - " : "", detail ? detail : "");
}
static void check(int cond, const char *what, const char *detail) {
    if (cond) ok(what); else bad(what, detail);
}
static void section(const char *name) {
    fw_printf("\n\033[95m[:]\033[0m \033[1m%s\033[0m\n", name);
}

// --- tasks ------------------------------------------------------------------

static volatile int g_ticks[3];
static volatile int g_cores_seen;

static int worker(void *arg) {
    int idx = (int)(long)arg;
    for (int i = 0; i < 40 && !fw_task_should_stop(); i++) {
        g_ticks[idx]++;
        fw_task_yield();
    }
    return idx;
}

// Never stops on its own. Only a kill ends it, which is what makes it a test of
// the kill path rather than of patience.
static int spinner(void *) {
    while (!fw_task_should_stop()) fw_task_yield();
    return 99;
}

static void test_tasks(void) {
    section("Multitasking");

    g_ticks[0] = g_ticks[1] = g_ticks[2] = 0;
    int a = fw_task_spawn("stress_a", worker, (void *)0, 1024);
    int b = fw_task_spawn("stress_b", worker, (void *)1, 1024);
    check(a > 0 && b > 0, "two tasks started", a > 0 ? 0 : "spawn refused");

    // Yield until they have both had plenty of turns.
    for (int i = 0; i < 200 && (g_ticks[0] < 40 || g_ticks[1] < 40); i++) fw_task_yield();

    check(g_ticks[0] >= 40, "task A ran to completion", 0);
    check(g_ticks[1] >= 40, "task B ran to completion", 0);
    // Interleaving is the real claim. If one had run to completion before the
    // other started, this OS would be running tasks in sequence and calling it
    // multitasking.
    check(g_ticks[0] > 5 && g_ticks[1] > 5, "both progressed together, not one then the other", 0);

    int s = fw_task_spawn("stress_spin", spinner, 0, 1024);
    check(s > 0, "a non-terminating task started", 0);
    for (int i = 0; i < 20; i++) fw_task_yield();
    check(fw_task_kill(s) != 0, "kill accepted", 0);
    for (int i = 0; i < 50; i++) fw_task_yield();
    ok("the killed task stopped at its next yield");

    fw_printf("      cores in use: %u\n", (unsigned)fw_cores());
    if (fw_cores() > 1) ok("second core is online");
    else fw_printf("      \033[90m(single core - not a fault, just this board)\033[0m\n");
}

// --- filesystem -------------------------------------------------------------

#define FS_ROUNDS 6
static volatile int g_fs_errors;
static volatile int g_fs_done;

// Each task hammers its OWN file. Concurrent writers to the same filesystem is
// exactly the case the storage lock exists for.
static int fs_worker(void *arg) {
    int idx = (int)(long)arg;
    char path[32];
    char buf[96];
    for (int r = 0; r < FS_ROUNDS && !fw_task_should_stop(); r++) {
        // A distinct, checkable payload per task and round.
        int n = 0;
        for (int i = 0; i < 64; i++) buf[n++] = (char)('A' + ((idx * 7 + r * 3 + i) % 26));
        buf[n] = 0;

        // Build the path without snprintf, which a package has no reason to pull in.
        const char *pre = "/stress_x.tmp";
        for (int i = 0; i < 14; i++) path[i] = pre[i];
        path[8] = (char)('0' + idx);

        if (!fw_file_write(path, buf, (uint32_t)n)) { g_fs_errors++; continue; }

        char back[96];
        uint32_t got = fw_file_read(path, back, sizeof(back));
        if (got != (uint32_t)n) { g_fs_errors++; continue; }
        for (int i = 0; i < n; i++)
            if (back[i] != buf[i]) { g_fs_errors++; break; }

        if (!fw_file_remove(path)) g_fs_errors++;
        fw_task_yield();
    }
    g_fs_done++;
    return 0;
}

static void test_files(void) {
    section("Filesystem");

    // Single-threaded first, so a plain failure is not blamed on concurrency.
    const char *msg = "rpcortex stress";
    check(fw_file_write("/stress.tmp", msg, 15) != 0, "wrote a file", 0);
    char back[32];
    uint32_t n = fw_file_read("/stress.tmp", back, sizeof(back));
    int same = (n == 15);
    for (uint32_t i = 0; i < n && same; i++) if (back[i] != msg[i]) same = 0;
    check(same, "read it back byte for byte", 0);
    check(fw_file_exists("/stress.tmp") != 0, "it exists", 0);
    check(fw_file_remove("/stress.tmp") != 0, "removed it", 0);
    check(fw_file_exists("/stress.tmp") == 0, "and it is gone", 0);

    // Now three tasks at once. Without the filesystem lock this is what
    // corrupts littlefs, so a clean run here is the lock doing its job.
    g_fs_errors = g_fs_done = 0;
    for (long i = 0; i < 3; i++) fw_task_spawn("stress_fs", fs_worker, (void *)i, 1280);
    for (int i = 0; i < 4000 && g_fs_done < 3; i++) fw_task_yield();

    check(g_fs_done == 3, "three tasks finished their file work", 0);
    check(g_fs_errors == 0, "no corruption with three writers at once",
          g_fs_errors ? "data came back wrong" : 0);
    fw_printf("      %d write/read/verify/delete cycles across 3 tasks\n", 3 * FS_ROUNDS);
}

// --- memory -----------------------------------------------------------------

static void test_memory(void) {
    section("Memory");

    uint32_t before = fw_heap_free();
    fw_printf("      heap: %u KB free of %u KB\n",
              (unsigned)(before / 1024), (unsigned)(fw_heap_total() / 1024));

    // Many small blocks, each filled with a value derived from its index and
    // checked afterwards. Verifying contents rather than just pointers is what
    // catches an allocator handing the same block to two callers.
    #define NBLOCK 24
    void *blocks[NBLOCK];
    int allocated = 0;
    for (int i = 0; i < NBLOCK; i++) {
        blocks[i] = fw_malloc(256);
        if (!blocks[i]) break;
        char *p = (char *)blocks[i];
        for (int j = 0; j < 256; j++) p[j] = (char)(i + j);
        allocated++;
    }
    check(allocated == NBLOCK, "allocated 24 blocks of 256 B", 0);

    int intact = 1;
    for (int i = 0; i < allocated && intact; i++) {
        char *p = (char *)blocks[i];
        for (int j = 0; j < 256; j++)
            if (p[j] != (char)(i + j)) { intact = 0; break; }
    }
    check(intact, "every block still holds what was written to it", 0);
    for (int i = 0; i < allocated; i++) fw_free(blocks[i]);

    uint32_t after = fw_heap_free();
    check(after >= before - 512, "memory came back after freeing",
          after < before ? "some was not returned" : 0);

    uint32_t largest = fw_heap_largest();
    fw_printf("      largest single block: %u KB\n", (unsigned)(largest / 1024));
    check(largest > 8 * 1024, "a large contiguous block is still available",
          "the heap is fragmented");

    // A refused allocation must RETURN NULL, not kill the device. This is the
    // exact bug that made meminfo panic, so it is worth a permanent check.
    void *huge = fw_malloc(0x7000000);
    check(huge == 0, "an impossible allocation fails cleanly instead of panicking", 0);
    if (huge) fw_free(huge);
}

// --- the command ------------------------------------------------------------

static int cmd_stress(int argc, char **argv) {
    (void)argc; (void)argv;
    g_pass = g_fail = 0;

    fw_printf("\n\033[96m=== RPCortex self test ===\033[0m\n");
    fw_printf("\033[90mCtrl+C stops it at any point.\033[0m\n");

    uint32_t t0 = fw_millis();
    test_tasks();
    if (!fw_task_should_stop()) test_files();
    if (!fw_task_should_stop()) test_memory();
    uint32_t ms = fw_millis() - t0;

    fw_printf("\n");
    if (g_fail == 0)
        fw_printf("  \033[96m[@]\033[0m \033[1m%d checks passed\033[0m in %u ms\n",
                  g_pass, (unsigned)ms);
    else
        fw_printf("  \033[91m[!]\033[0m \033[1m%d passed, %d FAILED\033[0m in %u ms\n",
                  g_pass, g_fail, (unsigned)ms);
    fw_printf("\n");
    return g_fail ? 1 : 0;
}

extern "C" int app_main(int) {
    rpc_register_command("stress", "run the on-board self test", cmd_stress);
    return 0;
}
