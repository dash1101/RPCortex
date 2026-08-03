// bench — RPCMark, the cross-version benchmark.
//
// The point is an apples-to-apples number against the MicroPython build, so the
// workloads are chosen to be expressible identically in both and to be dominated
// by the thing being measured rather than by I/O:
//
//   integer     a tight arithmetic loop. On MicroPython this is bytecode
//               dispatch; here it is the ALU. The largest gap, and the least
//               interesting one, but it is what people mean by "how much faster".
//   memory      walking and summing a buffer. Measures the memory path rather
//               than the interpreter, so the gap is smaller and more honest.
//   function    call overhead, which is where an interpreter really pays.
//   string      building and scanning text — what most code actually does.
//   filesystem  write, read back, delete. Both versions run littlefs on the same
//               flash, so this should be CLOSE to equal; a large gap here means
//               something other than the language is being measured.
//
// Iteration counts are fixed rather than time-based, so both versions do exactly
// the same amount of work.

#include "rpc_app.h"

RPC_APP_VER("bench", "1.0");

#define INT_ITER    200000
#define MEM_BYTES     8192
#define MEM_PASSES      40
#define CALL_ITER   100000
#define STR_ITER      2000
#define FS_ITER         20

static volatile uint32_t g_sink;      // stops the compiler deleting the work

static uint32_t bench_int(void) {
    uint32_t t0 = fw_millis();
    uint32_t acc = 0;
    for (uint32_t i = 1; i <= INT_ITER; i++) {
        acc += i;
        acc ^= (acc >> 3);
        acc += (i * 7);
    }
    g_sink = acc;
    return fw_millis() - t0;
}

static uint32_t bench_mem(void) {
    uint8_t *buf = (uint8_t *)fw_malloc(MEM_BYTES);
    if (!buf) return 0;
    for (int i = 0; i < MEM_BYTES; i++) buf[i] = (uint8_t)i;

    uint32_t t0 = fw_millis();
    uint32_t sum = 0;
    for (int p = 0; p < MEM_PASSES; p++)
        for (int i = 0; i < MEM_BYTES; i++) sum += buf[i];
    uint32_t ms = fw_millis() - t0;

    g_sink = sum;
    fw_free(buf);
    return ms;
}

// Deliberately not inlinable: the call itself is what is being timed.
static uint32_t __attribute__((noinline)) leaf(uint32_t a, uint32_t b) {
    return a + b;
}

static uint32_t bench_call(void) {
    uint32_t t0 = fw_millis();
    uint32_t acc = 0;
    for (uint32_t i = 0; i < CALL_ITER; i++) acc = leaf(acc, i);
    g_sink = acc;
    return fw_millis() - t0;
}

static uint32_t bench_str(void) {
    char buf[64];
    uint32_t t0 = fw_millis();
    uint32_t hits = 0;
    for (uint32_t i = 0; i < STR_ITER; i++) {
        // Built by hand rather than with snprintf: MicroPython's string
        // formatting is a different code path from its string handling, and this
        // measures the second.
        int n = 0;
        uint32_t v = i;
        char tmp[12];
        int t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        buf[n++] = 'i'; buf[n++] = 't'; buf[n++] = 'e'; buf[n++] = 'm';
        while (t) buf[n++] = tmp[--t];
        buf[n] = 0;
        for (int k = 0; k < n; k++) if (buf[k] == '7') hits++;
    }
    g_sink = hits;
    return fw_millis() - t0;
}

static uint32_t bench_fs(void) {
    char data[256];
    for (int i = 0; i < 256; i++) data[i] = (char)('a' + i % 26);

    uint32_t t0 = fw_millis();
    for (int i = 0; i < FS_ITER; i++) {
        if (!fw_file_write("/tmp/bench.tmp", data, sizeof(data))) break;
        char back[256];
        fw_file_read("/tmp/bench.tmp", back, sizeof(back));
        fw_file_remove("/tmp/bench.tmp");
    }
    return fw_millis() - t0;
}

// Fixed work over elapsed time, so higher is faster. The reference figures are
// the matching bench.py timings on a v1.0 Pico 2 W at stock clock, which is what
// makes a score mean "this many times v1.0" instead of an arbitrary constant.
static uint32_t score_of(uint32_t ms, uint32_t reference_ms) {
    if (ms == 0) ms = 1;
    return (reference_ms * 200) / ms;
}

static void line(const char *name, uint32_t ms, uint32_t ref, uint32_t *total) {
    uint32_t sc = score_of(ms, ref);
    *total += sc;
    fw_printf("  %-12s %6u ms   \033[96m%5u\033[0m\n", (unsigned)0 ? "" : name,
              (unsigned)ms, (unsigned)sc);
}

static int cmd_bench(int argc, char **argv) {
    (void)argc; (void)argv;

    fw_printf("\n\033[96m=== RPCMark ===\033[0m\n");
    fw_printf("\033[90mSame workload as the MicroPython build, so the numbers compare.\033[0m\n\n");
    fw_printf("  %-12s %9s   %5s\n", "TEST", "TIME", "SCORE");
    fw_printf("  ------------------------------\n");

    uint32_t total = 0;
    fw_progress("bench: integer");
    line("integer",  bench_int(),  18700, &total);
    fw_progress("bench: memory");
    line("memory",   bench_mem(),   9400, &total);
    fw_progress("bench: function");
    line("function", bench_call(),  8100, &total);
    fw_progress("bench: string");
    line("string",   bench_str(),   6200, &total);
    fw_progress("bench: filesystem");
    line("filesys",  bench_fs(),     420, &total);
    fw_progress("bench: done");

    fw_printf("  ------------------------------\n");
    fw_printf("  %-12s %9s   \033[96m\033[1m%5u\033[0m\n", "TOTAL", "", (unsigned)total);
    fw_printf("\n  \033[90mA v1.0 device scores about 1000. Run tools/bench.py there.\033[0m\n\n");
    return 0;
}

extern "C" int app_main(int) {
    rpc_register_command("bench", "RPCMark - compare against the v1 build", cmd_bench);
    return 0;
}
