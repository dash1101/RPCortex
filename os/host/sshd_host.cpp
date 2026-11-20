// The device's SSH server — os/shell/sshd.cpp itself, unchanged — run on the
// host, so a real OpenSSH client can drive the code a board will run: the
// network task, the session shells on their own tasks, the rings between them,
// the output sink and its spill, Ctrl+C into the right interrupt domain, and
// every way a connection can end.
//
// What is real: sshd.cpp, the protocol core, the scheduler (task.cpp, on
// ucontext stacks), the accounts (users.cpp), identity (ident.cpp), the
// filesystem guard (fsguard.cpp, protect.cpp), the output layer (out.cpp), the
// interrupt domains, the line editor, the registry and the log ring.
//
// What is faked, here: sockets (nettcp over the host's TCP, one look and no
// waiting, as on the device), storage (a directory on the host), and the
// shell's line runner — a handful of commands that exist to be checked:
//
//   whoami, isadmin, readsecret (reads /os/users.cfg through the guard),
//   echo, big N, sleep N (polls intr_check), false, exit/logout,
//   locked N (prints N bytes while holding a lock: the spill),
//   bgjob (starts a task that records whose it is), ssh server off
//
//   sshd_host <port> <root dir> <authorized_keys>
//
// Accounts: root/rootpw (admin, the key in authorized_keys), bob/bobpw, and
// guest with no password — with the same key in its authorized_keys, to prove
// a password-less account is refused by key as well. root is signed in at the
// "console" (the host's main task) throughout.
#include "task.h"
#include "ident.h"
#include "users.h"
#include "perms.h"
#include "registry.h"
#include "logring.h"
#include "out.h"
#include "lock.h"
#include "interrupt.h"
#include "fsguard.h"
#include "session.h"
#include "shell.h"
#include "nettcp.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string>

int sshd_command(int argc, char **argv);
void sshd_boot(void);

extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen) {
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY);
    if (read(fd, out, len) != (ssize_t)len) return -1;
    *olen = len;
    return 0;
}

// --- the scheduler's seam: ucontext, a real clock, generous stacks ------------------
//
// The tasks' own stacks are sized for an M33. Here every context gets 256 KB of
// its own instead, because x86-64 frames under AddressSanitizer are several
// times larger; the device's stack figures come from -fstack-usage, not this.
#define CTX_MAX 256
#define HOST_STACK (256 * 1024)
static ucontext_t g_ctx[CTX_MAX];
static int        g_ctx_n;
static TaskEntry  g_entry[CTX_MAX];
static void ctx_trampoline(int idx) { g_entry[idx](); }

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
static uint64_t now_us(void) {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
uint32_t task_now_ms(void) { return (uint32_t)(now_us() / 1000u); }
uint32_t task_now_us(void) { return (uint32_t)now_us(); }
uint32_t task_core_count(void) { return 1; }
uint32_t task_this_core(void) { return 0; }
void task_watchdog_start(void) {}
void task_watchdog_feed(void) {}
uint32_t task_main_stack_headroom(void) { return 1024 * 1024; }
uint32_t task_main_stack_size(void) { return 1024 * 1024; }
void task_main_stack_paint(void) {}
uint32_t task_main_stack_used(void) { return 0; }
void task_stack_overflow(const char *n, uint32_t) { fprintf(stderr, "stack overflow in %s\n", n); abort(); }
void task_stack_guard_set(const void *, uint32_t) {}
void task_app_mem_apply(const TaskAppMem *) {}
void task_slot_recycled(int) {}
extern "C" bool sandbox_guard_stack(int, void **, unsigned *) { return false; }
extern "C" unsigned task_irq_save(void) { return 0; }
extern "C" void task_irq_restore(unsigned) {}
static uint32_t g_seed = 11;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// --- storage: a directory --------------------------------------------------------------

static std::string g_root;
static std::string P(const char *p) { return g_root + (p[0] == '/' ? "" : "/") + p; }

uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap) {
    FILE *f = fopen(P(name).c_str(), "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    return more ? 0 : (uint32_t)n;               // "absent or too big", as on the device
}
bool storage_write_file(const char *name, const uint8_t *data, uint32_t len) {
    FILE *f = fopen(P(name).c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}
bool storage_append_file(const char *name, const uint8_t *data, uint32_t len) {
    FILE *f = fopen(P(name).c_str(), "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}
bool storage_mkdir(const char *path) { return mkdir(P(path).c_str(), 0700) == 0 || errno == EEXIST; }
bool storage_remove(const char *path) { return unlink(P(path).c_str()) == 0 || rmdir(P(path).c_str()) == 0; }
bool storage_stat(const char *path, bool *is_dir, uint32_t *size) {
    struct stat st;
    if (stat(P(path).c_str(), &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (size) *size = (uint32_t)st.st_size;
    return true;
}
void persist_save_dirty(void) {}

// --- sockets ------------------------------------------------------------------------------

bool net_is_connected(void) { return true; }
static int g_open;           // handles handed out and not yet closed; see net_pkg_tcp_close
static int H(int fd) { return fd + 1000; }
static int F(int h)  { return h - 1000; }

int net_pkg_tcp_listen(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) { close(fd); return -1; }
    g_open++;
    return H(fd);
}
int net_tcp_accept_now(int l) {
    int fd = accept4(F(l), nullptr, nullptr, SOCK_NONBLOCK);
    if (fd < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    g_open++;
    return H(fd);
}
int net_tcp_read_now(int h, void *buf, unsigned cap) {
    ssize_t r = recv(F(h), buf, cap, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;                                   // closed, or reset
}
int net_tcp_write_now(int h, const void *buf, unsigned len) {
    ssize_t w = send(F(h), buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w >= 0) return (int)w;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}
bool net_tcp_drained(int h) {
    int q = 0;
    return ioctl(F(h), SIOCOUTQ, &q) != 0 || q == 0;
}
void net_tcp_adopt(int) {}
void net_tcp_peer(int, char *out, unsigned cap) { snprintf(out, cap, "127.0.0.1"); }
// Every handle handed out is counted until it is closed: on the device a
// handle never closed is one of six slots gone for good, so the test checks the
// count comes back to one (the listener) once every connection has ended.
int  net_pkg_tcp_close(int h) { close(F(h)); g_open--; return 0; }

// --- the session, as session.cpp does it ---------------------------------------------------

const char *session_user(void) { return ident_user(); }
bool session_is_remote(void) { return ident_remote(); }
void session_attach(const char *user, const TaskTerm *term, uint16_t dom) {
    char home[TASK_CWD_MAX];
    snprintf(home, sizeof(home), "/home/%s", user);
    bool d = false;
    ident_attach(user, term, dom, storage_stat(home, &d, nullptr) && d ? home : "/");
}
void session_logout(void) {
    TaskShell *ts = task_shell();
    if (ts->own_user && ts->term && ts->term->hangup) ts->term->hangup(ts->term->ctx);
}
bool session_confirm(const char *) { return false; }

// --- the shell's line runner, faked ---------------------------------------------------------

void shell_prompt_text(char *out, unsigned cap) {
    snprintf(out, cap, "%s@host:%s> ", session_user(), task_shell()->cwd);
}
bool shell_complete(void *, const char *, uint32_t, uint32_t, char *, uint32_t) { return false; }

static RpcLock g_some_lock;
static const char kPattern[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/";

static void print_pattern(long n) {
    char buf[512];
    for (long j = 0; j < n; ) {
        uint32_t k = 0;
        while (k < sizeof(buf) && j < n) { buf[k++] = (j % 64 == 63) ? '\n' : kPattern[j % 64]; j++; }
        out_write(buf, k);
    }
}

static int bgjob(void *) {
    // Started from a remote session: whose is it?
    for (int i = 0; i < 20; i++) task_yield();
    char line[64];
    int n = snprintf(line, sizeof(line), "%s %s\n", ident_user(), ident_remote() ? "remote" : "console");
    storage_write_file("/tmp/bgjob", (const uint8_t *)line, (uint32_t)n);
    return 0;
}

int shell_run_input(char *line) {
    char *argv[8]; int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 8; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (!argc) return 0;
    const char *c = argv[0];
    if (!strcmp(c, "whoami"))  { out_multi("%s", session_user()); return 0; }
    if (!strcmp(c, "isadmin")) {
        bool ok = perms_allows(session_user(), users_is_admin(session_user()), LEVEL_ADMIN, false);
        out_multi("%s", ok ? "admin" : "not admin");
        return ok ? 0 : 1;
    }
    if (!strcmp(c, "readsecret")) {
        if (!fs_allow_read("/os/users.cfg")) return 1;
        out_multi("allowed");
        return 0;
    }
    if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) { if (i > 1) out_write(" ", 1); out_write(argv[i], (uint32_t)strlen(argv[i])); }
        out_write("\n", 1);
        return 0;
    }
    if (!strcmp(c, "big") && argc > 1) { print_pattern(atol(argv[1])); return 0; }
    if (!strcmp(c, "locked") && argc > 1) {
        // Output while a lock is held: it must be set aside, never waited on.
        LockGuard g(&g_some_lock);
        print_pattern(atol(argv[1]));
        return 0;
    }
    if (!strcmp(c, "sleep") && argc > 1) {
        uint32_t until = task_now_ms() + (uint32_t)atoi(argv[1]) * 1000u;
        while ((int32_t)(task_now_ms() - until) < 0) {
            if (intr_check()) { out_warn("Interrupted."); intr_clear(); return 130; }
            task_sleep_ms(10);
        }
        out_multi("slept");
        return 0;
    }
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "exit") || !strcmp(c, "logout")) { session_logout(); return 0; }
    if (!strcmp(c, "bgjob")) {
        storage_mkdir("/tmp");
        return task_spawn("bgjob", nullptr, bgjob, nullptr, TASK_STACK_SHELL, AFFINITY_ANY) > 0 ? 0 : 1;
    }
    if (!strcmp(c, "ssh") && argc >= 3) return sshd_command(argc, argv);
    out_err("'%s' is not a command or executable file.", c);
    return 1;
}

// --- the "console" --------------------------------------------------------------------------

static void put_file(const char *path, const char *text) {
    storage_write_file(path, (const uint8_t *)text, (uint32_t)strlen(text));
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: sshd_host port root authorized_keys\n"); return 2; }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_root = argv[2];
    task_init("console");

    for (const char *d : { "/os", "/home", "/home/root", "/home/root/.ssh", "/home/bob",
                           "/home/guest", "/home/guest/.ssh", "/tmp" })
        storage_mkdir(d);
    put_file("/os/users.cfg", "the account file\n");
    FILE *k = fopen(argv[3], "rb");
    static char keys[8192];
    size_t kn = k ? fread(keys, 1, sizeof(keys) - 1, k) : 0;
    if (k) fclose(k);
    keys[kn] = 0;
    put_file("/home/root/.ssh/authorized_keys", keys);
    put_file("/home/guest/.ssh/authorized_keys", keys);     // and still refused

    users_add("root", "rootpw", true, false);
    users_add("bob", "bobpw", false, false);
    users_add("guest", nullptr, false, true);
    ident_console_set("root");

    reg_set("SSH.Port", argv[1]);
    char *on[] = { (char *)"ssh", (char *)"server", (char *)"on" };
    if (sshd_command(3, on) != 0) return 1;
    char *fp[] = { (char *)"ssh", (char *)"server", (char *)"fingerprint" };
    sshd_command(3, fp);
    printf("READY\n");

    // The console idles, and passes on the log ring as it fills: the test
    // reads the server's own record of logins and refusals.
    uint32_t seen = 0;          // lines ever logged, dropped ones included
    int open_said = -1;
    for (;;) {
        task_yield();
        if (g_open != open_said) { open_said = g_open; fprintf(stderr, "open sockets: %d\n", g_open); }
        uint32_t n = log_count(), total = log_dropped() + n;
        uint32_t fresh = total - seen;
        if (fresh > n) fresh = n;
        for (uint32_t i = n - fresh; i < n; i++) {
            const LogLine *l = log_at(i);
            if (l) fprintf(stderr, "log: %s\n", l->text);
        }
        seen = total;
    }
}
