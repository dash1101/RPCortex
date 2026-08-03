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
#define RPC_API_MINOR 3

typedef struct {
    uint32_t magic;          // RPC_APP_MAGIC
    uint16_t api_major;
    uint16_t api_minor;
    char     name[24];
    char     version[12];    // the package's own version, e.g. "1.0.0"
    uint32_t flags;          // reserved (e.g. "registers commands", "wants core1")
} RpcAppHeader;

#define RPC_APP_MAGIC 0x52504341u   // 'RPCA'

// RPC_APP(name) defaults the version to "1.0"; RPC_APP_VER names it explicitly.
// The version travels IN the app, the way v1's pkg.ver lived in package.cfg, so
// the package manager reads it from the header rather than being told separately.
#define RPC_APP_VER(appname, ver)                                             \
    __attribute__((section(".rpc_app_header"), used))                         \
    const RpcAppHeader rpc_app_header = {                                     \
        RPC_APP_MAGIC, RPC_API_MAJOR, RPC_API_MINOR, appname, ver, 0           \
    }
#define RPC_APP(appname) RPC_APP_VER(appname, "1.0")

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

// --- exported services (API 1.3) -------------------------------------------
//
// Tasks. A package's task is scheduled like any other and uses the second core
// when the board has one — a package never has to know how many there are.
typedef int (*TaskFn)(void *arg);
int      fw_task_spawn(const char *name, TaskFn fn, void *arg, uint32_t stack);
void     fw_task_yield(void);
void     fw_task_sleep_ms(uint32_t ms);
int      fw_task_self(void);
// Non-zero once this task has been asked to stop. Any loop that runs for a
// while must check it, or it cannot be killed.
int      fw_task_should_stop(void);
int      fw_task_kill(int pid);
uint32_t fw_cores(void);

// Files. Paths are absolute.
int      fw_file_write(const char *path, const void *data, uint32_t len);
uint32_t fw_file_read(const char *path, void *buf, uint32_t cap);
int      fw_file_remove(const char *path);
int      fw_file_exists(const char *path);

// Memory. fw_heap_largest is the biggest single allocation available right now —
// free bytes do not predict whether the next allocation succeeds, this does.
uint32_t fw_heap_free(void);
uint32_t fw_heap_total(void);
uint32_t fw_heap_largest(void);

// Entry point. Called once when the app is loaded. A command-only package can do
// its registration here and return; a foreground app can run its loop.
int app_main(int arg);

#ifdef __cplusplus
}
#endif
#endif  // RPC_APP_H
