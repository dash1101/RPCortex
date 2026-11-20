// sudo, end to end on the host: the gate run_one puts in front of every
// command, the password, the grace, who can be asked and who cannot, the file
// guard's line between a user's command and the OS's own writes, and link's
// AUTH.
//
// Everything here is the real code: sudo.cpp, perms.cpp, ident.cpp, users.cpp,
// fsguard.cpp, protect.cpp, persist.cpp and the registry, on real tasks
// switching on ucontext stacks (the console's shell task, background jobs, a
// trusted service, remote logins with and without a pty). Only the prompt is
// faked, by a reader that answers from a script and counts how often it was
// asked, and the clock, which the test moves.
//
// run_one itself (shell/shell.cpp) is not compiled here: it pulls in the whole
// shell. What it does about sudo is sudo_gate, which is.
#include "task.h"
#include "ident.h"
#include "users.h"
#include "perms.h"
#include "sudo.h"
#include "fsguard.h"
#include "interrupt.h"
#include "out.h"
#include "logring.h"
#include "registry.h"
#include "persist.h"

#include <map>
#include <string>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ucontext.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

// --- the scheduler's platform seam, on ucontext (as identity_test does) ----------

#define CTX_MAX 64
static ucontext_t g_ctx[CTX_MAX];
static int        g_ctx_n;
static TaskEntry  g_entry[CTX_MAX];
static void ctx_trampoline(int idx) { g_entry[idx](); }

// Each context gets a generous stack of its own (as sshd_host does): x86-64
// frames under AddressSanitizer are several times an M33's, and the device's
// stack figures come from -fstack-usage, not from here.
#define HOST_STACK (256 * 1024)
void *task_ctx_init(void *, TaskEntry entry) {
    if (g_ctx_n >= CTX_MAX) abort();
    int idx = g_ctx_n++;
    g_entry[idx] = entry;
    getcontext(&g_ctx[idx]);
    g_ctx[idx].uc_stack.ss_sp   = malloc(HOST_STACK);
    g_ctx[idx].uc_stack.ss_size = HOST_STACK;
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
static uint32_t g_now = 1000;
uint32_t task_now_ms(void) { return g_now; }
uint32_t task_now_us(void) { return g_now * 1000u; }
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
static uint32_t g_seed = 5;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// --- storage, in a map: what persist.cpp writes ------------------------------------

static std::map<std::string, std::string> g_files;
bool storage_write_file(const char *name, const uint8_t *data, uint32_t len) {
    g_files[name] = std::string((const char *)data, len);
    return true;
}
uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap) {
    auto it = g_files.find(name);
    if (it == g_files.end()) return 0;
    uint32_t n = (uint32_t)it->second.size() < cap ? (uint32_t)it->second.size() : cap;
    memcpy(buf, it->second.data(), n);
    return n;
}
bool storage_mkdir(const char *) { return true; }

// --- the prompt, scripted ------------------------------------------------------------

static const char *g_answers[8];
static int  g_nanswers, g_next;
static int  g_reads;           // how many times a person was asked
static char g_last_prompt[128];
static int  g_pauses;

static void answer(std::initializer_list<const char *> a) {
    g_nanswers = 0; g_next = 0;
    for (const char *s : a) g_answers[g_nanswers++] = s;
}
static void fake_read(const char *prompt, char *buf, unsigned cap) {
    g_reads++;
    snprintf(g_last_prompt, sizeof(g_last_prompt), "%s", prompt);
    snprintf(buf, cap, "%s", g_next < g_nanswers ? g_answers[g_next++] : "");
}
static void fake_pause(uint32_t) { g_pauses++; }

// --- the gate, as run_one uses it ------------------------------------------------------

struct Cmd { const char *name; CmdLevel level; uint8_t forms; };
// The levels these commands are REGISTERED with; perms_test checks the shell's
// sources say the same.
static const Cmd kCmds[] = {
    {"ls", LEVEL_USER, FORMS_NONE},       {"rm", LEVEL_USER, FORMS_NONE},
    {"reboot", LEVEL_ADMIN, FORMS_NONE},  {"pkg", LEVEL_ADMIN, FORMS_PKG},
    {"rmuser", LEVEL_SYSTEM, FORMS_NONE}, {"usermod", LEVEL_SYSTEM, FORMS_NONE},
    {"passwd", LEVEL_USER, FORMS_PASSWD}, {"ssh", LEVEL_USER, FORMS_SSH},
    {"service", LEVEL_ADMIN, FORMS_SERVICE}, {"autonomy", LEVEL_ADMIN, FORMS_AUTONOMY},
};

struct Gate {
    bool ok = false;           // the command would run
    bool elevated = false;     // ...with sudo
    int  asked = 0;            // times a password was asked for
    std::string said;          // what it printed
};

static Gate gate(const char *line) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", line);
    char *raw[16];
    int n = 0;
    for (char *t = strtok(buf, " "); t && n < 16; t = strtok(nullptr, " ")) raw[n++] = t;
    char **argv = raw;
    int argc = n;
    bool prefixed = false;
    if (n > 1 && !strcmp(raw[0], "sudo")) { prefixed = true; argv++; argc--; }
    const Cmd *c = nullptr;
    for (const Cmd &k : kCmds) if (!strcmp(k.name, argv[0])) c = &k;
    if (!c) { printf("  (test has no command %s)\n", argv[0]); abort(); }

    Gate g;
    char cap[1024];
    int before = g_reads;
    bool capturing = out_capture_begin_all(cap, sizeof(cap));
    g.ok = sudo_gate(c->level, c->forms, argc, argv, prefixed, &g.elevated);
    uint32_t got = capturing ? out_capture_end() : 0;
    cap[got < sizeof(cap) ? got : sizeof(cap) - 1] = 0;
    g.said = cap;
    g.asked = g_reads - before;
    return g;
}

static bool said(const Gate &g, const char *what) { return g.said.find(what) != std::string::npos; }

static bool logged(const char *what) {
    for (uint32_t i = 0; i < log_count(); i++) {
        const LogLine *l = log_at(i);
        if (l && strstr(l->text, what)) return true;
    }
    return false;
}

static void run_until(volatile bool *flag) {
    for (int i = 0; i < 400 && !*flag; i++) task_yield();
}

// --- background work ---------------------------------------------------------------------

static volatile bool g_job_done;
static Gate g_job;
static bool g_job_elevated_at_start;
static int bg_job(void *line) {
    g_job_elevated_at_start = task_shell()->elevated;
    g_job = gate((const char *)line);
    g_job_done = true;
    return 0;
}
static Gate in_job(const char *line) {
    g_job_done = false;
    task_spawn("job", nullptr, bg_job, (void *)line, TASK_STACK_SHELL, AFFINITY_ANY);
    run_until(&g_job_done);
    ck(g_job_done, "the job ran");
    return g_job;
}

// A service, as bg_entry starts one from /etc/services.cfg.
static int service_job(void *line) {
    task_shell()->elevated = true;
    return bg_job(line);
}

// --- remote logins ------------------------------------------------------------------------

static bool g_pty = true;
static int  no_key(void *) { return -1; }
static bool term_size(void *, uint16_t *c, uint16_t *r) {
    if (!g_pty) return false;
    *c = 80; *r = 24;
    return true;
}
static TaskTerm g_term = { no_key, nullptr, term_size, nullptr };

struct Remote {
    const char *user;
    uint16_t dom;
    const char *lines[6];
    Gate got[6] = {};
    bool started_elevated = false;
    bool done = false;
};
static int remote_session(void *arg) {
    Remote *r = (Remote *)arg;
    ident_attach(r->user, &g_term, r->dom, "/");
    r->started_elevated = task_shell()->elevated;
    for (int i = 0; i < 6 && r->lines[i]; i++) {
        if (!strcmp(r->lines[i], "-k")) { sudo_forget(); continue; }
        if (!strcmp(r->lines[i], "job")) {
            // A job started from this login: same session, no terminal.
            r->got[i] = in_job("sudo pkg install greet");
            continue;
        }
        r->got[i] = gate(r->lines[i]);
    }
    r->done = true;
    return 0;
}
static Remote *remote(Remote *r) {
    r->dom = intr_domain_new();
    task_spawn("ssh", nullptr, remote_session, r, TASK_STACK_SHELL, AFFINITY_ANY);
    run_until(&r->done);
    ck(r->done, "the remote session ran");
    return r;
}

int main(void) {
    task_init("console");
    task_mark_shell();
    sudo_set_reader(fake_read);
    sudo_set_pause(fake_pause);
    users_add("root", "rootpw", true, false);
    users_add("ada", "adapw", true, false);
    users_add("bob", "bobpw", false, false);
    users_add("guest", nullptr, false, true);
    ident_console_set("root");

    // --- at the console's prompt, as root ------------------------------------------------
    Gate g = gate("pkg install greet");
    ck(!g.ok && g.asked == 0, "pkg install without sudo is refused, and nothing is asked");
    ck(said(g, "'pkg install' changes the system. Run it with sudo: sudo pkg install greet"),
       "and it says so the way a person can act on");

    g = gate("pkg list");
    ck(g.ok && !g.elevated && g.asked == 0, "pkg list runs, without sudo and without asking");
    g = gate("reboot");
    ck(g.ok && g.asked == 0, "an everyday admin command runs as before");
    g = gate("ls /");
    ck(g.ok && !g.elevated && g.asked == 0, "and a user one");

    answer({"rootpw"});
    g = gate("sudo pkg install greet");
    ck(g.ok && g.elevated && g.asked == 1, "sudo pkg install asks once and runs with sudo");
    ck(strstr(g_last_prompt, "Password for root, to run 'pkg install'") != nullptr,
       "the prompt names the account and what it is for");

    g = gate("sudo rmuser bob");
    ck(g.ok && g.elevated && g.asked == 0, "within the grace it does not ask again");
    g_now += SUDO_GRACE_MS - 1000;
    g = gate("sudo usermod bob admin on");
    ck(g.ok && g.asked == 0, "each use starts the time again");
    g_now += SUDO_GRACE_MS - 1000;
    g = gate("sudo rmuser bob");
    ck(g.ok && g.asked == 0, "so a run of commands is asked for once");
    g_now += SUDO_GRACE_MS + 1;
    answer({"rootpw"});
    g = gate("sudo rmuser bob");
    ck(g.ok && g.asked == 1, "after five quiet minutes it asks again");

    sudo_forget();
    answer({"rootpw"});
    g = gate("sudo rmuser bob");
    ck(g.ok && g.asked == 1, "sudo -k forgets: the next one asks");

    g = gate("rmuser bob");
    ck(!g.ok && g.asked == 0 && said(g, "Run it with sudo"),
       "the grace does not make a command without sudo run with it");

    sudo_forget();
    int pauses = g_pauses;
    answer({"nope", "wrong", "rootpwX"});
    g = gate("sudo rmuser bob");
    ck(!g.ok && g.asked == 3, "three wrong passwords and it is refused");
    ck(said(g, "Wrong password."), "each one says so");
    ck(g_pauses - pauses == 3, "with a pause after each");
    ck(logged("auth: refused 'root' three times, to run 'rmuser'"), "and the refusal is logged");
    ck(said(g, "safeboot"), "and at the console it says where the way back is");

    answer({"nope", "rootpw"});
    g = gate("sudo rmuser bob");
    ck(g.ok && g.asked == 2, "a wrong one then the right one goes through");

    sudo_forget();
    answer({"rootpw"});
    ck(sudo_validate() == 0 && g_reads > 0, "sudo -v asks now");
    int reads = g_reads;
    g = gate("sudo pkg remove greet");
    ck(g.ok && g_reads == reads, "and the next sudo does not");

    // sudo in front of an everyday command makes it run with sudo, for a file
    // in /os.
    g = gate("sudo rm /os/old.txt");
    ck(g.ok && g.elevated, "sudo rm runs with sudo");

    // --- the file guard: a user's command against the OS's own writes ---------------------
    {
        sudo_forget();
        ck(!fs_may_change("/os/registry.cfg") && !fs_may_change("/etc/startup.cfg") &&
           !fs_may_change("/os/pkg/novad1.app") && !fs_may_change("/os") && !fs_may_change("/*") &&
           !fs_may_change("/"), "root without sudo cannot change /os, /etc, or what reaches them");
        ck(fs_may_change("/home/root/notes.txt") && fs_may_change("/tmp/x") &&
           fs_may_change("/home/bob/notes.txt"), "but everything else is as before");
        ck(fs_may_read("/os/registry.cfg") && fs_may_read("/os/users.cfg"), "and reading is as before");
        ck(fs_change_needs_sudo("/etc/startup.cfg") && !fs_change_needs_sudo("/home/root/x"),
           "the refusal is one sudo would fix");
        char cap[512];
        out_capture_begin_all(cap, sizeof(cap));
        bool ok = fs_allow_change("/etc/startup.cfg");
        uint32_t n = out_capture_end();
        cap[n < sizeof(cap) ? n : sizeof(cap) - 1] = 0;
        ck(!ok && strstr(cap, "/etc/startup.cfg is part of the system. Run the command with sudo."),
           "and says so");
        // The OS's own writes are not a user's command: they never ask the guard.
        reg_set("WiFi.Auto", "true");
        g_files.clear();
        persist_save_registry();
        persist_save_users();
        ck(g_files.count("/os/registry.cfg") && g_files.count("/os/users.cfg") &&
           g_files["/os/registry.cfg"].find("WiFi.Auto") != std::string::npos,
           "while the registry and the account file are still saved");
        task_shell()->elevated = true;
        ck(fs_may_change("/etc/startup.cfg") && fs_may_change("/os/pkg/x.app"), "with sudo, root may");
        task_shell()->elevated = false;
        ident_console_set("bob");
        task_shell()->elevated = true;
        ck(!fs_may_change("/etc/startup.cfg") && !fs_may_read("/os/users.cfg"),
           "an ordinary account is held back whatever the flag says");
        ck(!fs_change_needs_sudo("/etc/startup.cfg"), "and sudo would not help it");
        task_shell()->elevated = false;
        ident_console_set("root");
    }

    // --- somebody who is not an admin ----------------------------------------------------
    ident_console_set("bob");
    reads = g_reads;
    g = gate("sudo pkg list");
    ck(!g.ok && g_reads == reads && said(g, "bob is not an admin. sudo cannot grant"),
       "sudo by an ordinary account is refused without asking");
    ck(logged("sudo: bob is not an admin"), "and logged");
    g = gate("pkg list");
    ck(!g.ok && said(g, "'pkg list' needs an admin account; signed in as bob"), "an admin command is refused");
    g = gate("sudo ls");
    ck(!g.ok && g_reads == reads, "even an everyday one in front of sudo");
    g = gate("ls");
    ck(g.ok, "an everyday one runs");
    g = gate("passwd");
    ck(g.ok && !g.elevated && g.asked == 0, "bob may change his own password");
    g = gate("passwd root");
    ck(!g.ok, "not root's");
    ck(sudo_validate() == 1, "sudo -v is refused");
    ident_console_set("guest");
    g = gate("passwd");
    ck(!g.ok, "a guest with no password cannot give itself one");
    ck(sudo_check_password("x") == SUDO_NOT_ADMIN, "AUTH from a guest is refused as not an admin");

    // --- an admin with no password to prove ------------------------------------------------
    // (users_set_nopass refuses an admin; an account file edited by hand can say so)
    // Covered by the NOPASS guard: a guest is the case that exists.

    // --- root under autonomy: signed in, nobody typed anything ------------------------------
    ident_console_set("root");
    sudo_forget();
    reads = g_reads;
    g = gate("rmuser bob");
    ck(!g.ok && g_reads == reads, "being root is not enough: rmuser needs sudo");
    answer({"guess1", "guess2", "guess3"});
    g = gate("sudo rmuser bob");
    ck(!g.ok && g.asked == 3, "and sudo needs root's password, which autonomy never asked for");
    g = gate("autonomy off");
    ck(!g.ok && said(g, "sudo autonomy off"), "turning autonomy off is the same");
    g = gate("autonomy status");
    ck(g.ok, "looking at it is not");
    ck(!fs_may_change("/etc/services.cfg"), "nor can it change what starts at boot");

    // --- unattended work ----------------------------------------------------------------------
    sudo_forget();
    reads = g_reads;
    g = in_job("sudo pkg install greet");
    ck(!g.ok && g_reads == reads, "a background job with no grace is refused, and nobody is asked");
    ck(said(g, "sudo needs a password and this job has no terminal; run `sudo -v` first."),
       "it says what to do, in one line");
    g = in_job("pkg install greet");
    ck(!g.ok && said(g, "Run it with sudo"), "without sudo in front it is refused as at the prompt");

    answer({"rootpw"});
    ck(sudo_validate() == 0, "sudo -v at the console");
    reads = g_reads;
    g = in_job("sudo pkg install greet");
    ck(g.ok && g.elevated && g_reads == reads, "and a job of the same session uses its grace");

    // A service: started from /etc/services.cfg, it runs with sudo.
    sudo_forget();
    g_job_done = false;
    task_spawn("svc", nullptr, service_job, (void *)"pkg install greet", TASK_STACK_SHELL, AFFINITY_ANY);
    run_until(&g_job_done);
    ck(g_job.ok && g_job.elevated && g_job.asked == 0, "a service runs a system command without asking");

    // What a sudo'd command starts runs with that sudo; nothing else does.
    task_shell()->elevated = true;            // the console, inside `sudo <command>`
    g = in_job("pkg install greet");
    ck(g_job_elevated_at_start && g.ok && g.elevated, "a task started inside a sudo'd command inherits it");
    task_shell()->elevated = false;
    g = in_job("pkg install greet");
    ck(!g_job_elevated_at_start && !g.ok, "one started after it does not");

    // --- SSH ---------------------------------------------------------------------------------------
    sudo_forget();
    {
        // Root logged in with a terminal: sudo asks on it. The console's grace
        // is not this session's, and the other way round.
        answer({"rootpw"});
        ck(sudo_validate() == 0, "the console has a grace");
        static Remote r1 = {"root", 0, {"pkg install greet", "sudo pkg install greet",
                                         "sudo rmuser bob", "job", "-k", "sudo rmuser bob"}};
        answer({"rootpw", "rootpw"});
        g_pty = true;
        reads = g_reads;
        remote(&r1);
        ck(!r1.got[0].ok && said(r1.got[0], "Run it with sudo"), "over SSH, a system command needs sudo too");
        ck(r1.got[1].ok && r1.got[1].asked == 1, "sudo asks on the session's own terminal, despite the console's grace");
        ck(r1.got[2].ok && r1.got[2].asked == 0, "then its own grace holds");
        ck(r1.got[3].ok && r1.got[3].asked == 0, "and a job from that login uses it");
        ck(r1.got[5].ok && r1.got[5].asked == 1, "sudo -k there forgets it");
        ck(sudo_grace_valid(), "and leaves the console's alone");
        sudo_grace_forget(r1.dom);
        intr_domain_free(r1.dom);

        // Root running one command with no pty: `ssh dev 'sudo pkg install x'`.
        static Remote r2 = {"root", 0, {"sudo pkg install greet", "pkg list"}};
        g_pty = false;
        reads = g_reads;
        remote(&r2);
        ck(!r2.got[0].ok && g_reads == reads, "without a terminal sudo does not ask: the password would echo");
        ck(said(r2.got[0], "sudo needs a terminal to ask for the password; connect with ssh -t."),
           "and says to use ssh -t");
        ck(r2.got[1].ok, "everything else still works there");
        intr_domain_free(r2.dom);

        // A guest logged in remotely gets nothing from sudo.
        static Remote r3 = {"guest", 0, {"sudo pkg list"}};
        g_pty = true;
        reads = g_reads;
        remote(&r3);
        ck(!r3.got[0].ok && g_reads == reads && said(r3.got[0], "not an admin"), "a remote guest cannot sudo");
        intr_domain_free(r3.dom);

        // A login started from a task running with sudo (the SSH server, turned
        // on by `sudo ssh server on`) does not inherit it.
        static Remote r4 = {"root", 0, {"rmuser bob"}};
        task_shell()->elevated = true;
        remote(&r4);
        task_shell()->elevated = false;
        ck(!r4.started_elevated && !r4.got[0].ok, "a new login starts without sudo, whatever started its task");
        intr_domain_free(r4.dom);
        g_pty = true;
    }

    // --- link: no prompt, AUTH ------------------------------------------------------------------
    sudo_forget();
    {
        TaskShell *ts = task_shell();
        ts->noprompt++;
        ts->sudo_flags = 0;
        reads = g_reads;
        g = gate("sudo pkg install greet");
        ck(!g.ok && g_reads == reads, "in link mode sudo never prompts");
        ck((ts->sudo_flags & SUDO_F_BLOCKED) != 0, "and marks the line, so EXEC can answer ERR sudo");
        ck(said(g, "cannot ask for one here"), "and says why");
        pauses = g_pauses;
        ck(sudo_check_password("wrong") == SUDO_WRONG && g_pauses == pauses + 1, "AUTH with a wrong password is refused, after a pause");
        ck(sudo_check_password("") == SUDO_WRONG, "and with none");
        ck(sudo_check_password("rootpw") == SUDO_OK, "AUTH with the right one is accepted");
        ts->sudo_flags = 0;
        g = gate("sudo pkg install greet");
        ck(g.ok && g.elevated && g_reads == reads && !(ts->sudo_flags & SUDO_F_BLOCKED),
           "and the same request then goes through");
        ts->noprompt--;
        for (int i = 0; i < 3; i++) sudo_check_password("bad");
        ck(logged("auth: 3 wrong passwords in a row for 'root' from a host tool"), "repeated wrong AUTHs are logged");
        ident_console_set("guest");
        ck(sudo_check_password("") == SUDO_NOT_ADMIN, "a guest's AUTH is refused");
        ident_console_set("");
        ck(sudo_check_password("x") == SUDO_NOBODY, "and nobody's");
        ident_console_set("root");
    }

    // --- a maintenance boot: physical presence ------------------------------------------------
    sudo_forget();
    sudo_set_maintenance(true);
    {
        reads = g_reads;
        g = in_job("sudo pkg install greet");
        ck(!g.ok && g_reads == reads, "a background job is not let through, maintenance or not");
        task_shell()->noprompt++;
        g = gate("sudo pkg install greet");
        task_shell()->noprompt--;
        ck(!g.ok, "nor is link's EXEC: a program is not a person at the keys");
        g = gate("sudo rmuser bob");
        ck(g.ok && g.elevated && g_reads == reads, "at the console's own prompt, sudo does not ask");
        ck(said(g, "Maintenance boot"), "and says so");
        ck(logged("sudo: root without a password (maintenance boot)"), "and logs it");
        static Remote r5 = {"root", 0, {"sudo rmuser bob"}};
        answer({"rootpw"});
        remote(&r5);
        ck(r5.got[0].ok && r5.got[0].asked == 1, "a remote login is still asked");
        intr_domain_free(r5.dom);
        ident_console_set("guest");
        g = gate("sudo rmuser bob");
        ck(!g.ok, "and an ordinary account at the console gets nothing from it");
        ident_console_set("root");
    }
    sudo_set_maintenance(false);

    // --- the grace is per session and per person ------------------------------------------------
    sudo_forget();
    answer({"rootpw"});
    ck(sudo_validate() == 0, "root at the console has a grace");
    ident_console_set("ada");           // somebody else signs in at the console
    answer({"adapw"});
    reads = g_reads;
    g = gate("sudo rmuser bob");
    ck(g.ok && g_reads == reads + 1, "the next person at the console is asked for their own");
    ident_console_set("root");
    sudo_grace_forget_all();

    printf("  sudo: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
