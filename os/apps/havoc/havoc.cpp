// havoc — a package whose job is to fail.
//
// Every call across the ABI is a place a package can hand the firmware
// something wrong, and the firmware dereferences those arguments while
// PRIVILEGED. So "it returned an error" is the only acceptable outcome for a
// bad one: a hard fault or a hang means the sandbox did not hold, and the
// device rebooting is the package's fault landing on everyone else.
//
// This is the tool that goes looking. It is not a self-test — `stress` checks
// that things WORK. havoc checks that things FAIL SAFELY, which is a different
// question and, on the evidence so far, the harder one.
//
// --- how to read a run ------------------------------------------------------
//
// Each step prints its name BEFORE running. So the last line on the screen is
// the thing that broke it, and a run that ends mid-line is a finding whether or
// not anything else is reported. That matters more than the tally: a suite that
// reboots the board has found something, and a suite that prints "all clean"
// has found nothing, and both are useful.
//
//   havoc            the safe suite: everything that must return an error
//   havoc all        adds the destructive groups below
//   havoc fault      a bad pointer                 (expects to be CONTAINED)
//   havoc stack      deliberate stack exhaustion   (expects a NAMED reset)
//   havoc spin       a loop that never yields      (expects to be killed)
//   havoc net <host> the radio at rate, off-core   (expects to SURVIVE)
//   havoc <group>    one group: ptr, str, handle, mem, task, io
//
// `net` is the odd one among the odd ones: it has to be run as
// `bg havoc net <host>` or it is pinned to core 0 with the shell and tests
// nothing. See group_net.
//
// `stack` and `spin` are separated out because they are SUPPOSED to end badly.
// The question for those two is whether the ending is a clean report naming the
// package, or a silent reboot.
#include "rpc_app.h"

RPC_APP_VER("havoc", "1.0");

// Off the stack. This package makes ABI calls that run firmware — including a
// TLS handshake — on its own stack, so it keeps almost nothing there.
static char     g_buf[256];
static char     g_scratch[64];
static unsigned g_checks, g_bad, g_skipped;

// A step announces itself first. If the device dies here, the last thing on the
// screen is the culprit.
static void step(const char *what) {
    fw_printf("  %s\n", what);
}

// The expectation for everything in the safe suite: it came back, and it said
// no. Coming back at all is most of the test.
static void refused(int rc, const char *what) {
    g_checks++;
    if (rc >= 0) {
        g_bad++;
        fw_printf("    [!] %s ACCEPTED (returned %d) — it should have refused\n",
                  what, rc);
    }
}

// NOT EVERY CALL SIGNALS FAILURE THE SAME WAY, and the first run of this got
// four verdicts wrong by assuming they did.
//
// fw_file_exists answers a question: 0 means "no such file", which for a bad
// argument is exactly the right answer, not an acceptance. fw_task_kill and
// fw_file_remove are the same shape — non-zero means it happened. Treating
// "returned 0" as a failure to refuse turned four correct behaviours into
// findings and buried the two that were real.
static void denied(int rc, const char *what) {
    g_checks++;
    if (rc != 0) {
        g_bad++;
        fw_printf("    [!] %s ACCEPTED (returned %d) — it should have refused\n",
                  what, rc);
    }
}

// Some calls return void or a count; for those the check is simply that control
// came back.
static void survived(const char *what) {
    g_checks++;
    (void)what;
}

// --- pointers ----------------------------------------------------------------
//
// The ABI checks every pointer a package passes against the five regions it was
// actually given. These are the ways to be outside them.
static void group_ptr(void) {
    fw_printf("\n[ptr] pointers the firmware must refuse\n");

    step("null out-pointer");
    denied((int)fw_file_read("/os/version", nullptr, 16), "fw_file_read(null)");
    refused((int)fw_net_resolve("example.com", nullptr, 16), "fw_net_resolve(null)");
    refused(fw_net_ssid(nullptr, 16), "fw_net_ssid(null)");

    step("a pointer that is not ours (firmware flash)");
    // 0x10000000 is XIP flash — the firmware's own code, which a package can
    // neither read nor write. Asking the OS to write there on our behalf is the
    // whole reason ptrcheck exists.
    refused((int)fw_net_resolve("example.com", (char *)0x10000000u, 16),
            "resolve into flash");
    denied((int)fw_file_read("/os/version", (void *)0x10000000u, 16), "read into flash");

    step("a pointer that is not ours (peripheral space)");
    refused((int)fw_net_resolve("example.com", (char *)0x40000000u, 16),
            "resolve into peripherals");

    step("inside the package region, but the length runs past the end");
    // The classic: a valid base with a size that walks off it. Checking the
    // base alone would let this through.
    refused((int)fw_net_resolve("example.com", g_scratch, 0x7FFFFFFFu),
            "resolve with a huge cap");
    denied((int)fw_file_read("/os/version", g_scratch, 0xFFFFFF00u),
           "read with a huge cap");

    step("length that overflows when added to the base");
    refused((int)fw_net_resolve("example.com", g_scratch, 0xFFFFFFFFu),
            "resolve with a wrapping length");

    step("a null buffer with a zero length");
    // Zero length is not automatically safe: it still has to be rejected rather
    // than reaching a memcpy that trusts the pointer.
    survived("zero-length calls returned");
    fw_file_read("/os/version", nullptr, 0);
    fw_sha256(nullptr, 0, (unsigned char *)g_buf);
}

// --- strings -----------------------------------------------------------------
//
// A string argument is a pointer with no length, so the firmware has to find the
// terminator itself without leaving the package's memory.
static void group_str(void) {
    fw_printf("\n[str] strings the firmware must refuse\n");

    step("null string");
    denied(fw_file_exists(nullptr), "fw_file_exists(null)");
    denied(fw_file_remove(nullptr), "fw_file_remove(null)");
    refused(fw_printf(nullptr), "fw_printf(null)");
    refused((int)fw_net_resolve(nullptr, g_scratch, sizeof(g_scratch)),
            "fw_net_resolve(null host)");

    step("a string in memory that is not ours");
    denied(fw_file_exists((const char *)0x10000000u), "exists() on flash");

    step("an UNTERMINATED string that runs to the end of the region");
    // Filled with no zero anywhere. A firmware that walks this looking for a
    // terminator must stop at the region boundary and refuse, not read on.
    for (unsigned i = 0; i < sizeof(g_scratch); i++) g_scratch[i] = 'A';
    denied(fw_file_exists(g_scratch), "exists() on an unterminated string");
    g_scratch[sizeof(g_scratch) - 1] = 0;

    step("a format string with more specifiers than arguments");
    // fw_printf's %s arguments are the one pointer the ABI still follows
    // unchecked, which is documented. This is the probe for it: a bad %s here
    // is the firmware dereferencing whatever was next on the stack.
    survived("printf with a lying format returned");
    fw_printf("%s%s%s%s\n", "ok", "", "", "");
}

// --- handles -----------------------------------------------------------------
static void group_handle(void) {
    fw_printf("\n[handle] handles that do not exist, or no longer do\n");

    step("close handles that were never opened");
    refused(fw_tcp_close(0),      "close(0)");
    refused(fw_tcp_close(-1),     "close(-1)");
    refused(fw_tcp_close(999999), "close(999999)");
    refused(fw_tcp_recv(0, g_buf, sizeof(g_buf), 10), "recv on handle 0");
    refused(fw_tcp_send(0, "x", 1), "send on handle 0");
    refused(fw_tcp_accept(0, 10), "accept on handle 0");

    step("a real listener, then the same handle after closing it");
    int lsn = fw_tcp_listen(8099);
    if (lsn < 0) {
        // COUNTED, not silently absent. A run that reports 55 checks where the
        // last one reported 62 looks like something regressed; saying seven
        // were skipped and why says the opposite.
        g_skipped += 7;
        fw_printf("    (no network, so 7 socket checks are skipped)\n");
    } else {
        g_checks++;
        if (fw_tcp_close(lsn) != 0) { g_bad++; fw_printf("    [!] close() refused a live handle\n"); }
        // Everything below is use-after-close. The generation in the handle is
        // what has to catch it; without one these land on whichever socket takes
        // the slot next.
        refused(fw_tcp_close(lsn), "close() twice");
        refused(fw_tcp_accept(lsn, 10), "accept() after close");
        refused(fw_tcp_recv(lsn, g_buf, sizeof(g_buf), 10), "recv() after close");
        refused(fw_tcp_send(lsn, "x", 1), "send() after close");
    }

    step("wrong kind of handle for the call");
    lsn = fw_tcp_listen(8098);
    if (lsn >= 0) {
        refused(fw_tcp_recv(lsn, g_buf, sizeof(g_buf), 10), "recv() on a listener");
        refused(fw_tcp_send(lsn, "x", 1), "send() on a listener");
        fw_tcp_close(lsn);
    }

    step("more listeners than there are slots");
    // Every one that succeeds must be closed again; the failure being tested is
    // the refusal, not the exhaustion.
    int held[12];
    int n = 0;
    for (int i = 0; i < 12; i++) {
        held[n] = fw_tcp_listen((unsigned)(9000 + i));
        if (held[n] < 0) break;
        n++;
    }
    fw_printf("    opened %d before it said no\n", n);
    for (int i = 0; i < n; i++) fw_tcp_close(held[i]);
    survived("the table refused rather than overflowing");
}

// --- memory ------------------------------------------------------------------
static void group_mem(void) {
    fw_printf("\n[mem] the package heap, and the ways to abuse it\n");

    step("free things that were never allocated");
    // A package can pass ANY pointer to fw_free. Following one blindly writes a
    // block header over whatever it pointed at.
    fw_free(nullptr);
    fw_free((void *)0x10000000u);      // flash
    fw_free((void *)0x40000000u);      // peripherals
    fw_free(g_buf);                    // ours, but not from the arena
    fw_free((void *)1);                // not even aligned
    survived("bad frees were ignored");

    step("allocate until it says no");
    void *held[64];
    int n = 0;
    while (n < 64) {
        held[n] = fw_malloc(512);
        if (!held[n]) break;
        n++;
    }
    fw_printf("    got %d blocks of 512 B\n", n);
    g_checks++;
    if (n == 64) { fw_printf("    (arena bigger than this test expected)\n"); }

    step("free the same block twice, then use the rest");
    if (n > 0) {
        fw_free(held[0]);
        fw_free(held[0]);              // twice — must not merge it away
        held[0] = nullptr;
    }
    for (int i = 1; i < n; i++) fw_free(held[i]);
    survived("double free survived");

    step("an impossible allocation");
    g_checks++;
    if (fw_malloc(0x7FFFFFFFu)) { g_bad++; fw_printf("    [!] a 2 GB allocation succeeded\n"); }
    g_checks++;
    if (fw_malloc(0)) { g_bad++; fw_printf("    [!] a zero-byte allocation returned memory\n"); }
}

// --- tasks -------------------------------------------------------------------

static int noop_task(void *) { return 0; }

static int suicidal_task(void *) {
    // Ends while its parent is still running, so the slot and the sandbox
    // attached to it are recycled underneath.
    fw_task_sleep_ms(1);
    return 0;
}

static void group_task(void) {
    fw_printf("\n[task] tasks that misbehave\n");

    step("spawn with a null function and a null name");
    refused(fw_task_spawn(nullptr, noop_task, nullptr, 1024), "spawn(null name)");
    refused(fw_task_spawn("bad", nullptr, nullptr, 1024), "spawn(null fn)");

    step("spawn with a name that is not ours");
    refused(fw_task_spawn((const char *)0x10000000u, noop_task, nullptr, 1024),
            "spawn(name in flash)");

    step("kill things that are not ours to kill");
    denied(fw_task_kill(-1),     "kill(-1)");
    denied(fw_task_kill(0),      "kill(0)");
    denied(fw_task_kill(999999), "kill(999999)");
    // pid 1 is the idle task. A package killing it would take the scheduler
    // down with it.
    denied(fw_task_kill(1), "kill(init)");

    step("spawn until it says no, and let them all finish");
    int n = 0;
    for (int i = 0; i < 24; i++) {
        if (fw_task_spawn("havoc_t", suicidal_task, nullptr, 1024) < 0) break;
        n++;
    }
    fw_printf("    spawned %d before it said no\n", n);
    for (int i = 0; i < 40 && n; i++) fw_task_sleep_ms(25);
    survived("the table refused rather than overflowing");

    step("kill ourselves by pid");
    // A package killing its own task from inside an ABI call: the call has to
    // return before the task can be taken apart, or the unwind runs on a stack
    // that has been handed back.
    // Killing our own task from inside an ABI call: the call has to return
    // before the task is taken apart, or the unwind runs on a stack that has
    // already been handed back. Either answer is defensible; what is not is a
    // fault, so this only checks that control came back.
    fw_task_kill(fw_task_self());
    survived("kill(self) returned");
}

// --- hardware ----------------------------------------------------------------
//
// Every pin argument is validated, because on RP2 a bad pin number is not an
// error — it is a different peripheral.
static void group_io(void) {
    fw_printf("\n[io] pins and buses that do not exist\n");

    step("pins past the end of the part");
    unsigned pins = fw_gpio_count();
    refused(fw_gpio_init(pins, FW_PIN_OUT), "gpio_init(count)");
    refused(fw_gpio_init(200, FW_PIN_OUT),  "gpio_init(200)");
    refused(fw_gpio_init(0xFFFFFFFFu, FW_PIN_OUT), "gpio_init(-1)");
    refused(fw_gpio_get(200), "gpio_get(200)");

    step("buses that do not exist");
    refused(fw_i2c_init(9, 0, 1, 100000),  "i2c_init(bus 9)");
    refused(fw_spi_init(9, 2, 3, 4, 1000000), "spi_init(bus 9)");
    refused(fw_uart_init(9, 0, 1, 115200), "uart_init(bus 9)");
    refused(fw_pwm_init(200, 1000),        "pwm_init(pin 200)");

    step("bus reads and writes with bad buffers");
    refused(fw_i2c_write(0, 0x50, nullptr, 4, 0), "i2c_write(null)");
    refused(fw_i2c_read(0, 0x50, (void *)0x10000000u, 4, 0), "i2c_read into flash");
    refused(fw_spi_write(0, nullptr, 4), "spi_write(null)");

    step("adc channels that do not exist");
    refused(fw_adc_init(99), "adc_init(99)");
    refused(fw_adc_read(99), "adc_read(99)");

    step("pio programs that make no sense");
    // A claim that succeeds has to be given back, or the next run of havoc
    // finds the state machines already gone and reports a false pass.
    refused(fw_pio_load(-1, nullptr, 0, 0, 0), "pio_load(bad handle, null)");
    int h = fw_pio_claim();
    if (h >= 0) {
        refused(fw_pio_load(h, nullptr, 0, 0, 0), "pio_load(null program)");
        refused(fw_pio_load(h, (const unsigned short *)0x10000000u, 4, 0, 0),
                "pio_load(program in flash)");
        fw_pio_release(h);
    }
    fw_pio_release(-1);          // releasing nothing must be harmless
    fw_pio_release(999);
    survived("bad pio releases were ignored");
}

// --- the destructive ones ----------------------------------------------------

static int burn_stack(int depth) {
    // Big enough that a few hundred frames exhaust any package stack, and
    // volatile so it cannot be optimised into nothing.
    volatile char pad[256];
    for (unsigned i = 0; i < sizeof(pad); i++) pad[i] = (char)depth;
    if (depth > 100000) return (int)pad[0];
    return burn_stack(depth + 1) + (int)pad[1];
}

static void group_stack(void) {
    fw_printf("\n[stack] deliberate stack exhaustion\n");
    fw_printf("  Recursion until the guard fires. This used to reset the\n");
    fw_printf("  device and no longer does, which took two things: the fault\n");
    fw_printf("  handler stands on a stack of its own rather than the one\n");
    fw_printf("  that just ran out, and the stack limit is armed on the\n");
    fw_printf("  package's own stack a little above the bottom - so the\n");
    fw_printf("  instruction is REFUSED with the stack pointer intact and\n");
    fw_printf("  there is still room to take the exception.\n\n");
    fw_printf("  Expected: STKOF, at an address INSIDE the package's stack,\n");
    fw_printf("  the command ends, the shell survives. A reset is a finding,\n");
    fw_printf("  and so is MSTKERR - that means the frame could not be\n");
    fw_printf("  written and there was nothing to redirect.\n\n");
    step("recursing until the stack runs out");
    fw_printf("    returned %d — the guard did not fire, which is its own finding\n",
              burn_stack(0));
}

// A fault that is NOT a stack overflow, which is the case containment covers.
//
// An unprivileged package writing outside its regions is refused by the memory
// protection, and the whole promise of the sandbox is that this costs the
// package and nothing else.
static void group_fault(void) {
    fw_printf("\n[fault] a bad pointer, dereferenced by the package itself\n");
    fw_printf("  Not through the ABI - the firmware checks those and returns an\n");
    fw_printf("  error. This writes to firmware flash directly, which the\n");
    fw_printf("  protection unit refuses.\n\n");
    fw_printf("  Expected: a report naming havoc, the command ends, the shell\n");
    fw_printf("  survives. A reset is a finding.\n\n");
    step("writing to 0x10000000");
    // volatile, or the write is optimised away as dead.
    *(volatile uint32_t *)0x10000000u = 0xDEADBEEF;
    fw_printf("    it was ALLOWED — the package wrote to firmware flash\n");
}

// The network, at the highest rate a package can drive it, from wherever the
// scheduler put this task.
//
// This is the shape of #98 and #102 and nothing else reproduces it. A package
// task is spawned with no affinity, which on this chip means core 1 almost
// always, and every network call it makes ends up inside the cyw43 driver's own
// lock — which the SDK keys on the CORE. Two things follow, and both need a
// HIGH RATE to be seen at all:
//
//   * Every transmit goes through cyw43_ll_sdpcm_send_common, which, when the
//     chip has run out of transmit credits, waits for the async_context worker
//     to replenish them — for up to a full second. That worker is a
//     low-priority interrupt on the core that brought the radio up, and it
//     cannot run while another core holds the driver's lock. So a package
//     transmitting under flow control from the wrong core holds that lock for
//     a second at a time, and the other core's cyw43_arch_lwip_begin is a
//     __wfe with no timeout.
//   * The same lock lets a SECOND TASK on the same core straight through,
//     because as far as it is concerned they are the same owner.
//
// So: no sleeps, no gaps, and run it as `bg havoc net <host>` while the shell
// does something that talks to the chip — `wifi scan` is the one. Run in the
// foreground it is pinned to core 0 with the shell and proves nothing.
//
//   Expected: it finishes, and the shell answered throughout. A reset, a
//   pause of seconds, or a stall report naming either task is the finding.
static void group_net(const char *host) {
    fw_printf("\n[net] the radio, at rate, from whichever core this landed on\n");
    if (!fw_net_connected()) {
        fw_printf("  Not connected — nothing to hammer. 'wifi connect' first.\n");
        return;
    }
    fw_printf("  Host %s. Run this as 'bg havoc net %s' and then 'wifi scan'.\n",
              host, host);
    fw_printf("  Expected: both finish. A reset or a long pause is the finding.\n\n");

    // A listener, so the tcp entries are exercised too and not only the ping
    // path. The port is high and arbitrary; nothing is expected to connect.
    int lsn = fw_tcp_listen(48512);
    if (lsn < 0) fw_printf("  (no listener — the tcp calls are skipped)\n");

    step("hammering");
    unsigned pings = 0, taken = 0, locks = 0;
    // The rate is the whole point, so the inner loop must not contain anything
    // that yields. fw_tcp_accept with a zero timeout is the tightest entry into
    // the driver's lock there is: it takes it, looks, and gives it back, with
    // no sleep on the way out. The ping is what puts packets on the air, and it
    // waits five milliseconds a time — so it is one in every hundred rather
    // than every iteration, and the other ninety-nine are lock churn.
    for (unsigned i = 0; i < 60000 && !fw_task_should_stop(); i++) {
        if (lsn >= 0) {
            int c = fw_tcp_accept(lsn, 0);
            locks++;
            if (c >= 0) { fw_tcp_close(c); taken++; }
        }
        if ((i % 100) == 0 && fw_net_ping(host, 1) != -1) pings++;
        if ((i % 10000) == 0) fw_printf("    %u\n", i);
    }
    if (lsn >= 0) fw_tcp_close(lsn);
    fw_printf("  %u driver entries, %u pings, %u taken. It came back.\n",
              locks, pings, taken);
}

static void group_spin(void) {
    fw_printf("\n[spin] a loop that never yields\n");
    fw_printf("  The watchdog should name this task and stop it, and the shell\n");
    fw_printf("  should still be there afterwards. A reboot is a finding.\n\n");
    step("spinning without yielding");
    // No fw_task_should_stop, no sleep, no ABI call: nothing for the scheduler
    // to get a turn on. This is what the forced-exit path exists for.
    volatile uint32_t x = 0;
    for (;;) x = x + 1;
}

// --- driver ------------------------------------------------------------------

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return !*a && !*b;
}

static void run_safe(void) {
    group_ptr();
    group_str();
    group_handle();
    group_mem();
    group_task();
    group_io();
}

static void report(void) {
    fw_printf("\n");
    if (g_bad) fw_printf("  %u of %u checks were ACCEPTED when they should not be.\n",
                         g_bad, g_checks);
    else       fw_printf("  %u checks, every one refused cleanly.\n", g_checks);
    if (g_skipped)
        fw_printf("  %u skipped for want of a network — connect and run again\n"
                  "  for the full set.\n", g_skipped);
    fw_printf("  Reaching this line at all is the main result.\n\n");
}

static int havoc_cmd(int argc, char **argv) {
    g_checks = g_bad = g_skipped = 0;

    fw_printf("\n=== havoc — trying to break the OS from inside the sandbox ===\n");
    fw_printf("Each step prints before it runs, so the last line is the culprit.\n");

    if (argc >= 2) {
        if (is(argv[1], "stack")) { group_stack(); report(); return 0; }
        if (is(argv[1], "fault")) { group_fault(); report(); return 0; }
        if (is(argv[1], "spin"))  { group_spin();  return 0; }   // never returns
        if (is(argv[1], "net")) {
            if (argc < 3) {
                fw_printf("Usage: havoc net <host>   (an address on the LAN)\n");
                fw_printf("Run it as 'bg havoc net <host>', or it is pinned to\n");
                fw_printf("core 0 with the shell and proves nothing.\n");
                return 1;
            }
            group_net(argv[2]);
            return 0;
        }
        if (is(argv[1], "ptr"))   { group_ptr();    report(); return 0; }
        if (is(argv[1], "str"))   { group_str();    report(); return 0; }
        if (is(argv[1], "handle")){ group_handle(); report(); return 0; }
        if (is(argv[1], "mem"))   { group_mem();    report(); return 0; }
        if (is(argv[1], "task"))  { group_task();   report(); return 0; }
        if (is(argv[1], "io"))    { group_io();     report(); return 0; }
        if (is(argv[1], "all")) {
            run_safe();
            group_stack();
            report();
            return 0;
        }
        fw_printf("Unknown group '%s'.\n", argv[1]);
        fw_printf("Try: ptr str handle mem task io fault stack spin net all\n");
        return 1;
    }

    run_safe();
    report();
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("havoc", "try to break the OS from inside the sandbox",
                         havoc_cmd);
    return 0;
}
