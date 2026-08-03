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
    (void)argc; (void)argv;

    fw_printf("%s\n", RULE);
    fw_printf("RPCortex probe - paste this whole block back\n");
    fw_printf("%s\n", RULE);

    probe_cores();
    probe_timing();
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
