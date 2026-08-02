// Built against an API major the firmware does not implement. Must be REFUSED
// at load time, before any of its code runs.
#include "rpc_app.h"
#undef RPC_API_MAJOR
#define RPC_API_MAJOR 99
RPC_APP("badver");
extern "C" int app_main(int arg) { fw_printf("should never run\n"); return arg; }
