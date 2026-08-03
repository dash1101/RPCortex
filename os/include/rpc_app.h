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
#define RPC_API_MINOR 6

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

// --- the TUI (API 1.4) ------------------------------------------------------
//
// A package draws a full screen the same way the built-in apps do. Everything
// renders into a grid the firmware owns and only the DIFFERENCE reaches the
// terminal, so a package that repaints on every keystroke still costs one row
// of escape sequences rather than two kilobytes.
//
// Mouse events arrive through the same poll as keys, already decoded, with
// coordinates counted from zero.

typedef struct {
    unsigned char kind;    // 0 none, 1 key, 2 mouse
    int           key;     // a character, or one of FW_KEY_*
    unsigned char mouse;   // 0 down, 1 up, 2 drag, 3 wheel up, 4 wheel down
    unsigned short x, y;   // zero-based cell coordinates
    unsigned char ctrl, shift, alt;
} FwTuiEvent;

#define FW_KEY_UP    256
#define FW_KEY_DOWN  257
#define FW_KEY_LEFT  258
#define FW_KEY_RIGHT 259
#define FW_KEY_HOME  260
#define FW_KEY_END   261
#define FW_KEY_PGUP  262
#define FW_KEY_PGDN  263
#define FW_KEY_ESC   279

// Enter and leave full-screen mode. ALWAYS pair them: a terminal left with
// mouse reporting on sends escape sequences to the shell for every click
// afterwards, which looks like the device typing by itself.
void fw_tui_begin(void);
void fw_tui_end(void);

void fw_tui_size(int *w, int *h);
void fw_tui_clear(void);
void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg);
void fw_tui_box(int x, int y, int w, int h, const char *title,
                unsigned char attr, unsigned char fg);
void fw_tui_fill(int x, int y, int w, int h, char ch,
                 unsigned char attr, unsigned char fg);
// Send what changed since the last call.
void fw_tui_present(void);
// One event, or 0 when nothing is waiting. Non-blocking, so the caller owns its
// own frame rate and stays responsive.
int  fw_tui_poll(FwTuiEvent *out);
// Re-ask the terminal its size and force a full repaint. Returns 1 when the
// size changed, so an app can re-lay-out. A serial line carries no resize
// notification, so this is what Ctrl+L should call.
int  fw_tui_refresh(void);

#define FW_ATTR_NORMAL  0
#define FW_ATTR_BOLD    1
#define FW_ATTR_REVERSE 2
#define FW_ATTR_DIM     4
#define FW_ATTR_UNDER   8

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
// --- hardware ---------------------------------------------------------------
//
// Added at 1.6, because the first three packages that needed it (gpio, i2cscan,
// dht) found there was nothing here at all — and neither is there a way to
// build the Nova D1 suite without it.
//
// Deliberately thin. These are the SDK's own operations with the pin validated
// and the board's limits applied, not a device framework: a package that wants
// to drive a pin should not have to learn an abstraction first, and anything
// richer would have to be guessed at before there are packages to guess from.
//
// Every call validates its pin against what the board actually has. An out of
// range pin is refused rather than poked, because on RP2 a bad pin number is
// not an error, it is a different peripheral.

#define FW_PIN_IN     0
#define FW_PIN_OUT    1

#define FW_PULL_NONE  0
#define FW_PULL_UP    1
#define FW_PULL_DOWN  2

// How many GPIOs this board exposes. Packages that scan a range should ask
// rather than assume 30 — the count differs across RP2040, RP2350 and the
// wireless variants, where some pins belong to the radio.
unsigned fw_gpio_count(void);

// True when the pin is safe for a package to use. False for pins that are out
// of range, or that the board wires to something the OS owns.
int      fw_gpio_usable(unsigned pin);

int      fw_gpio_init(unsigned pin, int dir);          // FW_PIN_IN / FW_PIN_OUT
int      fw_gpio_pull(unsigned pin, int mode);         // FW_PULL_*
int      fw_gpio_put(unsigned pin, int value);
int      fw_gpio_get(unsigned pin);                    // 0/1, or -1 if refused

// ADC. Channels 0-3 are GPIO 26-29; the board's temperature sensor is on the
// channel fw_adc_temp_channel() reports, which is not the same number on every
// part. Returns the raw 12-bit reading, or -1.
int      fw_adc_init(unsigned channel);
int      fw_adc_read(unsigned channel);
unsigned fw_adc_temp_channel(void);

// I2C. `bus` is 0 or 1. Returns the number of bytes moved, or negative on
// error — which is what makes a bus scan possible: a write of zero bytes to an
// address either acknowledges or does not.
int      fw_i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned baud);
int      fw_i2c_write(unsigned bus, unsigned addr, const void *data, unsigned len, int nostop);
int      fw_i2c_read(unsigned bus, unsigned addr, void *buf, unsigned len, int nostop);
int      fw_i2c_deinit(unsigned bus);

// Microsecond timing. fw_task_sleep_ms yields, which is right for anything
// waiting on the world and wrong for a protocol that is timed in microseconds —
// a DHT's whole conversation is over in 5 ms and a yield in the middle of it
// loses the reading.
uint32_t fw_micros(void);
void     fw_busy_wait_us(uint32_t us);

uint32_t fw_heap_free(void);
uint32_t fw_heap_total(void);
uint32_t fw_heap_largest(void);

// Record a checkpoint that survives a crash. Anything printed is lost when the
// device hangs — it never leaves the USB buffer — so a long-running program
// should call this before each step it might not come back from. The last one
// recorded is shown in the crash report at the next boot.
void     fw_progress(const char *what);

// Entry point. Called once when the app is loaded. A command-only package can do
// its registration here and return; a foreground app can run its loop.
int app_main(int arg);

#ifdef __cplusplus
}
#endif
#endif  // RPC_APP_H
