// A package, not a one-shot: it registers a shell command when loaded, the way
// a v1 package added a command via programs.lp. Proves the ABI seam that makes
// an app extend the shell.
#include "rpc_app.h"
RPC_APP("greet");

static int greet_cmd(int argc, char **argv) {
    fw_printf("hello from the greet package (argc=%d)\n", argc);
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("greet", "say hello (from a loaded package)", greet_cmd);
    fw_log(0, "greet package registered its command");
    return 0;
}
