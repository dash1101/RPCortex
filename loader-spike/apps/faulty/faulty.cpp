// Dereferences a deliberately invalid pointer. The firmware must survive, name
// the fault, and return to the prompt.
#include "rpc_app.h"
RPC_APP("faulty");
extern "C" int app_main(int arg) {
    fw_printf("  about to touch 0x%08x\n", 0xF0000000u);
    volatile uint32_t *bad = (volatile uint32_t *)0xF0000000u;
    *bad = 0xdeadbeef;
    fw_printf("  survived (unexpected)\n");
    return arg;
}
