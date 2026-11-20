// Who a command runs as, now that there can be two sessions at once.
//
// The property that matters most in the SSH work: a guest logged in over SSH
// must be a guest, whoever is signed in at the console — and a remote root must
// not make the console's guest an admin either. Every permission check in the
// OS reduces to the questions asked here (session_user via ident_user,
// users_is_admin, perms_allows for LEVEL_ADMIN dispatch, and the filesystem
// guard), so this drives the REAL code for all of them — ident.cpp, users.cpp,
// perms.cpp, fsguard.cpp, protect.cpp, interrupt.cpp, task.cpp — with real
// tasks switching on ucontext stacks, the console's login on one and remote
// logins on others.
//
// Also here: what a job started from a remote login inherits (the user, the
// directory, the interrupt domain; never the terminal), and that one session's
// Ctrl+C cannot reach, or clear, another's.
#include "task.h"
#include "ident.h"
#include "users.h"
#include "perms.h"
#include "fsguard.h"
#include "interrupt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ucontext.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

// --- the scheduler's platform seam, on ucontext (as task_test does) -------------

#define CTX_MAX 32
static ucontext_t g_ctx[CTX_MAX];
static int        g_ctx_n;
static TaskEntry  g_entry[CTX_MAX];
static void ctx_trampoline(int idx) { g_entry[idx](); }

void *task_ctx_init(void *stack_top, TaskEntry entry) {
    if (g_ctx_n >= CTX_MAX) abort();
    int idx = g_ctx_n++;
    g_entry[idx] = entry;
    getcontext(&g_ctx[idx]);
    g_ctx[idx].uc_stack.ss_sp   = (char *)stack_top - TASK_STACK_SHELL;
    g_ctx[idx].uc_stack.ss_size = TASK_STACK_SHELL;
    g_ctx[idx].uc_link          = nullptr;
    makecontext(&g_ctx[idx], (void (*)())ctx_trampoline, 1, idx);
    return &g_ctx[idx];
}
void task_ctx_switch(void **save_sp, void *to_sp, volatile bool *live_out) {
    static ucontext_t root;
    ucontext_t *from = *save_sp ? (ucontext_t *)*save_sp : &root;
    *save_sp = from;
    volatile bool resumed = false;
    getcontext(from);
    if (resumed) return;
    resumed = true;
    if (live_out) *live_out = false;
    setcontext((ucontext_t *)to_sp);
}
extern "C" void lock_hw_init(void) {}
extern "C" void lock_hw_enter(void) {}
extern "C" void lock_hw_exit(void) {}
extern "C" unsigned lock_hw_core(void) { return 0; }
uint32_t task_now_ms(void) { return 0; }
uint32_t task_now_us(void) { return 0; }
uint32_t task_core_count(void) { return 1; }
uint32_t task_this_core(void) { return 0; }
void task_watchdog_start(void) {}
void task_watchdog_feed(void) {}
uint32_t task_main_stack_headroom(void) { return 1024 * 1024; }
uint32_t task_main_stack_size(void) { return 1024 * 1024; }
void task_main_stack_paint(void) {}
uint32_t task_main_stack_used(void) { return 0; }
void task_stack_overflow(const char *, uint32_t) { abort(); }
void task_stack_guard_set(const void *, uint32_t) {}
void task_app_mem_apply(const TaskAppMem *) {}
void task_slot_recycled(int) {}
extern "C" bool sandbox_guard_stack(int, void **, unsigned *) { return false; }
extern "C" unsigned task_irq_save(void) { return 0; }
extern "C" void task_irq_restore(unsigned) {}
static uint32_t g_seed = 7;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// --- what "may this task do X" means everywhere in the OS ---------------------------

// run_one's dispatch check for a LEVEL_ADMIN command, and `sudo`'s.
static bool may_run_admin(bool sudo) {
    const char *u = ident_user();
    return perms_allows(u, users_is_admin(u), LEVEL_ADMIN, sudo);
}

static const char *kSecret   = "/os/users.cfg";
static const char *kHostKey  = "/os/ssh/hostkey";
static const char *kRootKeys = "/home/root/.ssh/authorized_keys";

static int  dummy_key(void *) { return -1; }
static TaskTerm g_term = { dummy_key, nullptr, nullptr, nullptr };

// --- a remote login as guest, while root is at the console --------------------------

static uint16_t g_guest_dom;
static bool     g_job_done;

static int job_body(void *) {
    // Started from the guest's login: still the guest, in its directory, in its
    // Ctrl+C domain — and with no terminal of its own.
    ck(ident_remote() && !strcmp(ident_user(), "guest"), "a job inherits the remote user");
    ck(!strcmp(task_shell()->cwd, "/home/guest/work"), "a job inherits the remote directory");
    ck(task_shell()->intr_dom == g_guest_dom, "a job inherits the interrupt domain");
    ck(task_shell()->term == nullptr, "a job does not inherit the terminal");
    ck(!may_run_admin(false) && fs_session_restricted(), "a job of a guest login is a guest");
    // Run on after the login that started it has gone.
    for (int i = 0; i < 6; i++) task_yield();
    ck(!strcmp(ident_user(), "guest"), "still the guest after its login has ended");
    g_job_done = true;
    return 0;
}

static bool g_guest_done;
static int guest_login(void *) {
    ident_attach("guest", &g_term, g_guest_dom, "/home/guest");
    ck(!strcmp(ident_user(), "guest"), "the remote session is the guest");
    ck(fs_session_restricted(), "the guest is restricted");
    ck(!may_run_admin(false), "LEVEL_ADMIN commands refused");
    ck(!may_run_admin(true), "sudo does not change that");
    ck(!users_is_admin(ident_user()), "users_is_admin on the session says no");
    ck(!fs_may_read(kSecret) && !fs_may_change(kSecret), "the account file stays shut");
    ck(!fs_may_read("/os/device.key"), "the device key stays shut");
    ck(!fs_may_read(kHostKey) && !fs_may_change(kHostKey) && !fs_may_read("/os/ssh"),
       "the SSH host key stays shut");
    ck(!fs_may_change(kRootKeys) && !fs_may_read(kRootKeys), "root's authorized_keys cannot be written or read");
    ck(!fs_may_change("/home/root/.ssh") && !fs_may_change("/home/root/.SSH/x"),
       "nor root's .ssh directory, however it is spelled");
    ck(!fs_may_change("/home/root") && !fs_may_change("/home/*") && !fs_may_change("/home/root/*"),
       "nor root's home as a whole, nor a wildcard that could reach it");
    ck(!fs_may_change("/home/guest/../root/.ssh/authorized_keys"), "nor by walking up out of its own");
    ck(fs_may_change("/home/guest/.ssh/authorized_keys") && fs_may_read("/home/guest/.ssh/known_hosts"),
       "its own .ssh is its own");
    ck(fs_may_read("/home/root/notes.txt") && fs_may_read("/home/root"), "other people's ordinary files can be read");
    ck(!fs_may_change("/home/root/notes.txt") && !fs_may_change("/home/root/boot.rps"),
       "but not changed: root may run them");
    ck(fs_may_change("/home/guest/notes.txt") && fs_may_change("/tmp/x"), "its own and the shared ones can");
    // What runs at boot as the console's user: none of it may be changed.
    ck(!fs_may_change("/etc/startup.cfg") && !fs_may_change("/etc/services.cfg") &&
       !fs_may_change("/etc/tasks.cfg") && !fs_may_change("/etc"),
       "startup items, services and scheduled tasks stay shut");
    ck(!fs_may_change("/os/pkg/index.cfg") && !fs_may_change("/os/pkg/novad1.app") &&
       !fs_may_change("/os/ca.pem") && !fs_may_change("/OS/Pkg/x.app"),
       "installed packages and the trusted certificates stay shut");
    ck(fs_may_read("/etc/startup.cfg") && fs_may_read("/os/ca.pem"), "and can still be read");
    // A package it runs writes settings as it: only its own.
    ck(!perms_reg_write_ok("System.Autonomous", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       !perms_reg_write_ok("SSH.Server", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       !perms_reg_write_ok("Alias.exit", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       !perms_reg_write_ok("WiFi.Auto", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       !perms_reg_write_ok("User.Theme", ident_restricted(), ident_remote(), task_shell()->elevated),
       "a remote guest's packages cannot write device settings, nor the console user's own");
    ck(perms_reg_write_ok("Apps.Game_Score", ident_restricted(), ident_remote(), task_shell()->elevated),
       "but can write their own");

    // Its own directory, separate from the console's.
    snprintf(task_shell()->cwd, TASK_CWD_MAX, "/home/guest/work");

    // Ctrl+C: this session's, and only this session's.
    intr_raise();
    ck(intr_pending(), "a remote Ctrl+C is pending in its own session");
    task_yield();                       // the console looks, then comes back
    intr_clear();
    ck(!intr_pending(), "and clears");

    int job = task_spawn("job", nullptr, job_body, nullptr, TASK_STACK_SHELL, AFFINITY_ANY);
    ck(ident_may_stop(job), "the guest may stop its own job");
    ck(!ident_may_stop(1), "but not the console's tasks");
    for (int i = 0; i < 2; i++) task_yield();
    g_guest_done = true;
    return 0;
}

// --- a remote login as root, while a guest is at the console ------------------------

static bool g_root_done;
static uint16_t g_root_dom;
static int root_login(void *) {
    ident_attach("root", &g_term, g_root_dom, "/home/root");
    ck(!fs_session_restricted() && may_run_admin(false), "a remote root is root");
    ck(fs_may_read(kSecret) && fs_may_change(kRootKeys), "and may touch what root may");
    ck(!perms_reg_write_ok("SSH.Server", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       perms_reg_write_ok("WiFi.Auto", ident_restricted(), ident_remote(), task_shell()->elevated),
       "including any setting but the ones that need sudo");
    task_shell()->elevated = true;
    ck(perms_reg_write_ok("SSH.Server", ident_restricted(), ident_remote(), task_shell()->elevated),
       "and those with it");
    task_shell()->elevated = false;
    for (int i = 0; i < 3; i++) task_yield();     // the console checks itself meanwhile
    ck(!strcmp(ident_user(), "root"), "still root while the console is someone else");
    g_root_done = true;
    return 0;
}

static int empty_login(void *) {
    // A remote identity with no name must fail closed, not open.
    ident_attach("", &g_term, 0, "/");
    ck(fs_session_restricted() && !may_run_admin(true), "an empty remote user is restricted");
    ck(!fs_may_read(kSecret), "and cannot read the account file");
    return 0;
}

static int follower(void *) {
    // Started from the console: follows the console, whoever that is now.
    ck(!ident_remote(), "a console job has no identity of its own");
    ck(!strcmp(ident_user(), ident_console()), "a console job follows the console's login");
    return 0;
}

static void run_until(volatile bool *flag) {
    for (int i = 0; i < 200 && !*flag; i++) task_yield();
}

int main(void) {
    task_init("console");
    users_add("root", "pw", true, false);
    users_add("guest", nullptr, false, true);
    users_add("bob", "pw", false, false);

    // Root at the console.
    ident_console_set("root");
    ck(!fs_session_restricted() && may_run_admin(false), "the console is root");

    g_guest_dom = intr_domain_new();
    ck(g_guest_dom != 0, "a remote session gets an interrupt domain");
    task_spawn("ssh-guest", nullptr, guest_login, nullptr, TASK_STACK_SHELL, AFFINITY_ANY);
    task_yield();                          // the guest raises its Ctrl+C, then yields back
    // The guest's Ctrl+C is pending in ITS domain, not the console's.
    ck(intr_pending_in(g_guest_dom), "the remote flag is set");
    ck(!intr_pending(), "the console does not see a remote Ctrl+C");
    // ...and the console clearing its own flag (as the shell does before every
    // line) does not clear the remote one.
    intr_clear();
    ck(intr_pending_in(g_guest_dom), "the console's clear does not reach the remote session");
    // The console's own Ctrl+C does not reach the remote session either.
    intr_raise();
    ck(intr_pending(), "console Ctrl+C pending at the console");
    run_until(&g_guest_done);
    ck(g_guest_done, "the guest session ran");
    intr_clear();

    // While all that ran, the console stayed root, in its own directory.
    ck(!strcmp(ident_user(), "root") && !ident_remote(), "the console is still root");
    ck(!fs_session_restricted(), "and still unrestricted");

    // The guest's login ends; its job runs on with the identity it was given.
    intr_domain_free(g_guest_dom);
    ck(!intr_pending_in(g_guest_dom), "a freed domain is never pending");
    intr_raise_in(g_guest_dom);
    ck(!intr_pending_in(g_guest_dom), "and cannot be raised");
    uint16_t reused = intr_domain_new();
    intr_raise_in(reused);
    ck(!intr_pending_in(g_guest_dom), "a reused slot does not make the old handle pending");
    intr_domain_free(reused);
    run_until(&g_job_done);
    ck(g_job_done, "the job finished");

    // Now the other way round: a guest at the console, root logged in remotely.
    ident_console_set("guest");
    ck(fs_session_restricted() && !may_run_admin(false), "the console is now a guest");
    ck(!perms_reg_write_ok("System.Autonomous", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       !perms_reg_write_ok("Alias.ll", ident_restricted(), ident_remote(), task_shell()->elevated) &&
       perms_reg_write_ok("WiFi.Auto", ident_restricted(), ident_remote(), task_shell()->elevated),
       "a console guest's packages cannot reach the keys that grant access");
    g_root_dom = intr_domain_new();
    int rootpid = task_spawn("ssh-root", nullptr, root_login, nullptr, TASK_STACK_SHELL, AFFINITY_ANY);
    task_yield();                      // it attaches its identity first thing
    ck(!ident_may_stop(rootpid), "the console's guest may not stop a remote root's session");
    ck(!strcmp(ident_user(), "guest") && fs_session_restricted() && !fs_may_read(kSecret),
       "a remote root does not make the console's guest an admin");
    run_until(&g_root_done);
    intr_domain_free(g_root_dom);

    // A console job follows the console through a change of login.
    ident_console_set("bob");
    task_spawn("follow", nullptr, follower, nullptr, TASK_STACK_SHELL, AFFINITY_ANY);
    for (int i = 0; i < 4; i++) task_yield();

    task_spawn("empty", nullptr, empty_login, nullptr, TASK_STACK_SHELL, AFFINITY_ANY);
    for (int i = 0; i < 4; i++) task_yield();

    // Nobody at the console (boot, the login prompt): unrestricted, as before.
    ident_console_set("");
    ck(!fs_session_restricted(), "nobody at the console: boot code is not held back");

    printf("  identity: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
