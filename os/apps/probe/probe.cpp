// Probe — measure the things a host test cannot, and print them to paste back.
//
// Nearly all of this OS is provable on the host, which is the point of keeping
// core/ free of hardware headers. A handful of things are not, and they are
// exactly the ones that have caused trouble:
//
//   * whether tasks actually migrate between cores, and how often
//   * whether an unpinned task ever runs network code on the wrong core,
//     which is the condition the net_core_ok guard exists for and which
//     nothing on a quiet device currently reaches
//   * whether the microsecond calls are accurate enough for a bit-banged
//     protocol, which is what dht now depends on
//   * whether the hardware ABI is plumbed through at all
//   * what the last crash was, which no host test can know
//
// Every section prints a line even when it fails, because a missing line is
// ambiguous and a failed one is data.
#include "rpc_app.h"

RPC_APP_VER("probe", "1.1");

#define RULE "----------------------------------------------------------"

// --- core migration ---------------------------------------------------------
//
// A package task is spawned AFFINITY_ANY, so it may run on either core. How
// often it changes is the number that decides how urgent task migration is for
// the network lock: if an unpinned task almost never lands on core 1, the
// guard is enough; if it lands there half the time, it is not.

static volatile int  g_seen[2];
static volatile int  g_switches;
static volatile bool g_done;

static int migrant(void *) {
    int last = -1;
    for (int i = 0; i < 2000; i++) {
        int c = (int)fw_core_id();
        if (c >= 0 && c < 2) g_seen[c]++;
        if (last >= 0 && c != last) g_switches++;
        last = c;
        fw_task_yield();
    }
    g_done = true;
    return 0;
}

static void probe_cores(void) {
    fw_printf("CORES\n");
    fw_printf("  cores available     %u\n", fw_cores());

    g_seen[0] = g_seen[1] = 0;
    g_switches = 0;
    g_done = false;

    int pid = fw_task_spawn("probe-mig", migrant, 0, 2048);
    if (pid < 0) { fw_printf("  migration           could not spawn a task\n"); return; }

    for (int i = 0; i < 4000 && !g_done; i++) fw_task_sleep_ms(1);

    int total = g_seen[0] + g_seen[1];
    if (!total) { fw_printf("  migration           the task never reported\n"); return; }
    fw_printf("  samples on core 0   %d  (%d%%)\n", g_seen[0], g_seen[0] * 100 / total);
    fw_printf("  samples on core 1   %d  (%d%%)\n", g_seen[1], g_seen[1] * 100 / total);
    fw_printf("  core changes        %d in %d yields\n", g_switches, total);
    // WHY 0% ON CORE 1 IS NOT A FINDING when this is run from the prompt. A
    // package command executes on the SHELL task, and the shell is pinned to
    // core 0 — so the only honest answer here is 100/0, and reading it as "the
    // second core is dead" is the mistake this line exists to prevent.
    if (!g_seen[1])
        fw_printf("  (run as 'bg probe' to sample an unpinned task; a command\n"
                  "   runs on the shell, which is pinned to core 0)\n");
}

// --- microsecond timing -----------------------------------------------------
//
// dht times a 26 us pulse against a 70 us one and splits them at 40. That only
// works if fw_micros advances smoothly and fw_busy_wait_us waits roughly what
// it was asked for, so both are measured rather than assumed.

static void probe_timing(void) {
    fw_printf("TIMING\n");

    // Monotonic, and how fine the steps are. A clock that jumps in 10 us
    // increments cannot decode a 26 us pulse.
    uint32_t a = fw_micros();
    uint32_t steps = 0, last = a;
    for (int i = 0; i < 2000; i++) {
        uint32_t n = fw_micros();
        if (n != last) { steps++; last = n; }
        if ((int32_t)(n - a) < 0) { fw_printf("  micros              WENT BACKWARDS\n"); return; }
    }
    fw_printf("  micros steps        %u distinct values in 2000 reads\n", steps);

    // Accuracy of the busy wait, at the durations the DHT decoder cares about.
    static const uint32_t want[] = { 10, 26, 50, 70, 100, 1000 };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint32_t t0 = fw_micros();
        fw_busy_wait_us(want[i]);
        uint32_t took = fw_micros() - t0;
        fw_printf("  busy_wait %5u us  took %u us\n", want[i], took);
    }
}

// --- hardware ---------------------------------------------------------------

// What a call into the firmware costs.
//
// On RP2350 a package runs unprivileged, so every fw_* call is a supervisor
// call: an SVC through a veneer, a handler, a privilege change, and the same
// again on the way back. That is not free, and until now nobody knew what it
// cost — `bench` scores in the millions so it plainly is not costing anything
// that matters at that scale, but "not much" is not a number, and the number is
// what decides whether a package that has to bit-bang a protocol ever needs an
// exemption.
//
// Measured against the cheapest call there is. fw_micros does almost nothing
// but read a register, so what is left after subtracting the loop is the
// boundary itself rather than the work behind it.
static void probe_syscall(void) {
    fw_printf("SYSCALL\n");

    const uint32_t N = 20000;
    volatile uint32_t sink = 0;

    // The empty loop first, so its own cost can be taken back out. Volatile so
    // the compiler cannot decide the whole thing is pointless.
    uint32_t t0 = fw_micros();
    for (uint32_t i = 0; i < N; i++) sink += i;
    uint32_t empty = fw_micros() - t0;

    t0 = fw_micros();
    for (uint32_t i = 0; i < N; i++) sink += fw_micros();
    uint32_t called = fw_micros() - t0;

    if (called <= empty) {
        fw_printf("  per call            too fast to measure this way\n");
        return;
    }
    uint32_t net_ns = (uint32_t)(((uint64_t)(called - empty) * 1000u) / N);
    fw_printf("  %u calls            %u us total, %u us loop overhead\n",
              (unsigned)N, (unsigned)called, (unsigned)empty);
    fw_printf("  per call            ~%u ns\n", (unsigned)net_ns);

    // At 150 MHz a cycle is about 6.7 ns. Reporting cycles as well as
    // nanoseconds is what makes the figure comparable against the datasheet
    // rather than against a clock speed someone has to remember.
    fw_printf("  at %u MHz           ~%u cycles\n",
              (unsigned)(fw_clock_hz() / 1000000u),
              (unsigned)(((uint64_t)net_ns * fw_clock_hz()) / 1000000000ull));
    fw_printf("  (whether this went through the sandbox is what 'mpu' reports)\n");
    fw_printf("\n");
}

// Does a PIO state machine actually run?
//
// The instruction encoders are checked on a host against the datasheet's own
// tables, and the ABI plumbing is checked against a fake device — but the fake
// RECORDS programs, it does not execute them. Whether a state machine ever
// drove a pin has never been established, and it cannot be from a host: there
// is no silicon there to run it.
//
// Proving it needs no strip, no scope and no second board. A jumper wire
// between two pins is enough: PIO is told to drive one, the CPU reads the
// other, and the only way the level changes is if the state machine executed
// the instructions it was given.
//
//     jumper GP2 --- GP3, then: probe pio
//
// Skipped without the argument, since driving a pin somebody has something
// attached to is not a thing to do uninvited.
static unsigned parse_pin(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

static void probe_pio(unsigned out_pin, unsigned in_pin) {
    fw_printf("PIO\n");
    fw_printf("  jumper needed       GP%u --> GP%u\n", out_pin, in_pin);

    int h = fw_pio_claim();
    if (h < 0) { fw_printf("  claim               FAILED - no state machine free\n"); return; }

    // Two instructions: set the pin high, set it low, and wrap. The delays make
    // each level last long enough for a polling CPU to see it without needing
    // any timing precision at all.
    static const unsigned short kProg[] = {
        FW_PIO_SET(0, 1, 31),      // dst 0 is "pins"
        FW_PIO_SET(0, 0, 31),
    };
    if (fw_pio_load(h, kProg, 2, 0, 1) < 0) {
        fw_printf("  load                FAILED\n");
        fw_pio_release(h);
        return;
    }
    // set_base is the output pin, one pin wide. config_pins also sets the pin
    // direction and hands it to PIO, so nothing else has to.
    if (fw_pio_config_pins(h, 0, 0, out_pin, 1, 0, 0) != 0) {
        fw_printf("  config              FAILED\n");
        fw_pio_release(h);
        return;
    }
    // Slowed hard. The divider is 24.8 fixed point, so 256 is full speed and
    // this is roughly a thousandth of that — a few kilohertz of instruction
    // rate, with 31 cycles of delay on each instruction on top. Sampling from C
    // then cannot miss an edge, and the result says something about PIO rather
    // than about how fast this loop polls.
    fw_pio_config_clock(h, 256 * 1000);

    fw_gpio_init(in_pin, FW_PIN_IN);
    fw_pio_start(h);

    // Watch for both levels. Either alone could be the pin's resting state; it
    // is seeing it CHANGE that means instructions ran.
    int saw_high = 0, saw_low = 0;
    uint32_t t0 = fw_millis();
    while (fw_millis() - t0 < 500 && !(saw_high && saw_low)) {
        if (fw_gpio_get(in_pin)) saw_high = 1; else saw_low = 1;
    }
    fw_pio_stop(h);
    fw_pio_release(h);

    if (saw_high && saw_low)
        fw_printf("  RESULT              the pin toggled - a state machine really ran\n");
    else
        fw_printf("  RESULT              pin stuck %s - no jumper, wrong pins, or PIO did not run\n",
                  saw_high ? "high" : "low");
    fw_printf("\n");
}

static void probe_hardware(void) {
    fw_printf("HARDWARE\n");
    fw_printf("  gpio count          %u\n", fw_gpio_count());

    fw_printf("  reserved pins       ");
    int any = 0;
    for (unsigned p = 0; p < fw_gpio_count(); p++)
        if (!fw_gpio_usable(p)) fw_printf("%s%u", any++ ? "," : "", p);
    fw_printf("%s\n", any ? "" : "none");

    // The die temperature. A plausible number means the ADC path works end to
    // end; 0 or 4095 means it does not.
    unsigned ch = fw_adc_temp_channel();
    if (fw_adc_init(ch) == 0) {
        int raw = fw_adc_read(ch);
        int mv  = raw < 0 ? -1 : (raw * 3300) / 4096;
        fw_printf("  adc temp channel    raw %d  (%d mV)\n", raw, mv);
    } else {
        fw_printf("  adc temp channel    would not initialise\n");
    }

    // I2C brought up and taken down again on the usual pins. Nothing need be
    // attached: this is checking the ABI is wired through, not the bus.
    int r = fw_i2c_init(0, 4, 5, 100000);
    fw_printf("  i2c0 init (4,5)     %s\n", r == 0 ? "ok" : "refused");
    fw_printf("  pio state machines  %u free of %u\n", fw_pio_free(), fw_pio_count());
    if (r == 0) {
        int found = 0;
        // One-byte read: a zero-length write does not generate a transaction,
        // so it reports every address as present.
        for (unsigned addr = 0x08; addr <= 0x77; addr++) {
            unsigned char rx = 0;
            if (fw_i2c_read(0, addr, &rx, 1, 0) >= 1) found++;
        }
        fw_printf("  i2c0 devices        %d\n", found);
        fw_i2c_deinit(0);
    }
}

// --- jitter -----------------------------------------------------------------
//
// The number the whole PIO argument rests on. fw_busy_wait_us measured about
// 1 us of error on an idle device, which is fine for anything with tens of
// microseconds of tolerance. What it does NOT show is what happens when the
// USB stack and the radio are also running — a CPU loop can be interrupted and
// a state machine cannot, and that difference is invisible on a quiet board.
//
// So the same wait is measured twice: undisturbed, then while the device is
// doing as much as it can be made to do from here. The spread between them is
// the honest figure for what a bit-banged protocol has to tolerate.

static int noisy(void *) {
    // Something for the other core to be busy with. Printing is deliberate:
    // it drags in the USB stack, which is the interrupt most likely to land
    // in the middle of somebody's pulse.
    for (int i = 0; i < 400 && !fw_task_should_stop(); i++) {
        fw_printf("");
        fw_task_yield();
    }
    return 0;
}

static void measure_spread(const char *label, uint32_t us) {
    uint32_t lo = 0xffffffffu, hi = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t t0 = fw_micros();
        fw_busy_wait_us(us);
        uint32_t took = fw_micros() - t0;
        if (took < lo) lo = took;
        if (took > hi) hi = took;
    }
    fw_printf("  %-18s %u us asked, %u-%u seen  (spread %u us)\n",
              label, us, lo, hi, hi - lo);
}

static void probe_jitter(void) {
    fw_printf("JITTER\n");
    measure_spread("quiet, 10 us", 10);
    measure_spread("quiet, 50 us", 50);

    int pid = fw_task_spawn("probe-noise", noisy, 0, 2048);
    if (pid < 0) {
        fw_printf("  under load         could not spawn the load task\n");
        return;
    }
    measure_spread("busy, 10 us", 10);
    measure_spread("busy, 50 us", 50);
    fw_task_kill(pid);

    fw_printf("  A spread of a microsecond or two is fine for a DHT. Tens of\n");
    fw_printf("  microseconds means anything tighter than that needs PIO.\n");
}

// --- memory -----------------------------------------------------------------

static void probe_memory(void) {
    fw_printf("MEMORY\n");
    fw_printf("  heap free           %u bytes\n", (unsigned)fw_heap_free());
    fw_printf("  heap total          %u bytes\n", (unsigned)fw_heap_total());
    // The largest single block, which is the number that matters: a fragmented
    // heap reports plenty free and still cannot hand out 16 KB for a TLS run.
    fw_printf("  largest block       %u bytes\n", (unsigned)fw_heap_largest());
}

static int probe_cmd(int argc, char **argv) {
    // `probe pio <out> <in>` runs the loopback and nothing else, because it
    // drives a pin and that is not something to do as part of a general report.
    if (argc >= 2 && argv[1][0] == 'p') {
        unsigned a = argc >= 3 ? parse_pin(argv[2]) : 2;
        unsigned b = argc >= 4 ? parse_pin(argv[3]) : 3;
        probe_pio(a, b);
        return 0;
    }

    fw_printf("%s\n", RULE);
    fw_printf("RPCortex probe - paste this whole block back\n");
    fw_printf("%s\n", RULE);

    probe_cores();
    probe_timing();
    probe_syscall();
    probe_hardware();
    probe_jitter();
    probe_memory();

    fw_printf("%s\n", RULE);
    fw_printf("Also worth pasting: 'diag', and 'logdump' if anything looks wrong.\n");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("probe", "measure what host tests cannot, for reporting", probe_cmd);
    return 0;
}
