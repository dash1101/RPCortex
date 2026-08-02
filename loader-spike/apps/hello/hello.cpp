// The reference application. Exercises the three relocation types that real
// code produces: a call into the firmware (THM_CALL, out of range -> veneer),
// an internal call (THM_CALL or THM_JUMP24, in range), and addresses taken
// from a literal pool (ABS32).
#include "rpc_app.h"

RPC_APP("hello");

static const char kMsg[] = "hello from a loaded app";
static int   run_count;                       // .bss — proves zeroing
static const int kTable[4] = {2, 3, 5, 7};    // .rodata — proves ABS32

static int triple(int x) { return x * 3; }    // internal call

extern "C" int app_main(int arg) {
    run_count++;
    uint32_t t = fw_millis();
    void *scratch = fw_malloc(64);
    fw_printf("  %s\n", kMsg);
    fw_printf("  arg=%d run=%d triple=%d table[2]=%d\n",
              arg, run_count, triple(arg), kTable[2]);
    fw_printf("  millis=%u malloc=%s\n", (unsigned)t, scratch ? "ok" : "failed");
    fw_free(scratch);
    return run_count;
}
