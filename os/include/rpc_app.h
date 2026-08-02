// The app-facing ABI for RPCortex v2 (the real OS, not the spike).
//
// The one header an application includes. An app is a relocatable ELF object
// that never links against the firmware; it declares the services it wants as
// extern and the loader patches the call sites at load time from the symbol
// table the OS exports (firmware/api.cpp).
//
// This is a superset of the loader-spike ABI: same header/versioning mechanism,
// plus logging and — the important addition — rpc_register_command, which is how
// an installed app adds commands to the shell. That call is the C++ package
// system: a package is an app that registers commands when it runs.
#ifndef RPC_APP_H
#define RPC_APP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MAJOR: a symbol removed or changed — older-major apps refused.
// MINOR: a symbol added — older-minor apps still run (everything they want is
//        present); newer-minor apps refused (they may want something absent).
#define RPC_API_MAJOR 1
#define RPC_API_MINOR 1

typedef struct {
    uint32_t magic;          // RPC_APP_MAGIC
    uint16_t api_major;
    uint16_t api_minor;
    char     name[24];
    uint32_t flags;          // reserved (e.g. "registers commands", "wants core1")
} RpcAppHeader;

#define RPC_APP_MAGIC 0x52504341u   // 'RPCA'

#define RPC_APP(appname)                                                      \
    __attribute__((section(".rpc_app_header"), used))                         \
    const RpcAppHeader rpc_app_header = {                                     \
        RPC_APP_MAGIC, RPC_API_MAJOR, RPC_API_MINOR, appname, 0                \
    }

// --- exported services (API 1.1) -------------------------------------------
// Every entry is a permanent compatibility commitment. Adding one is a MINOR
// bump; changing or removing one is a MAJOR bump.
int      fw_printf(const char *fmt, ...);
uint32_t fw_millis(void);
void    *fw_malloc(size_t n);
void     fw_free(void *p);
void     fw_log(int level, const char *msg);            // 0 info 1 warn 2 error

// Register a shell command. The OS tags it with the calling app as owner and
// removes it automatically when the app unloads, so a package cannot leave a
// dangling command behind. Returns non-zero on success. This is the seam that
// makes an app a package.
typedef int (*RpcCommandFn)(int argc, char **argv);
int      rpc_register_command(const char *name, const char *help, RpcCommandFn fn);

// Entry point. Called once when the app is loaded. A command-only package can do
// its registration here and return; a foreground app can run its loop.
int app_main(int arg);

#ifdef __cplusplus
}
#endif
#endif  // RPC_APP_H
