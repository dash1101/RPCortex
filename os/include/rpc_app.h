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
#define RPC_API_MINOR 18

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
// Which core this task is on RIGHT NOW. Not stable: a task with no affinity
// moves between cores, which is the whole reason a package might want to ask.
uint32_t fw_core_id(void);

// Files. Paths are absolute.
int      fw_file_write(const char *path, const void *data, uint32_t len);
uint32_t fw_file_read(const char *path, void *buf, uint32_t cap);
int      fw_file_remove(const char *path);
int      fw_file_exists(const char *path);

// The longest command line fw_shell_run will take. Matches the shell's own.
#define RPC_SHELL_LINE_MAX 256

// Run a shell command line and, optionally, capture what it printed.
//
// The whole shell: pipes, `&&`, `||`, `;` and redirection all work, because
// this is the same runner that handles a line typed at the prompt. Pass a null
// `out` to run something for its effect and ignore the output.
//
// It runs with the SESSION's privileges, exactly as if the logged-in user had
// typed it — not with the package's. A package cannot use this to gain rights
// the person at the keyboard does not have.
//
// Returns the command's exit status, or -1 if the line was refused (too long,
// or a pointer that is not the package's).
int      fw_shell_run(const char *line, char *out, uint32_t cap);

// How busy the machine is, 0-100, sampled since anything last asked. Per CORE,
// so two cores each fully busy reads 100 rather than 200. Call it periodically
// and each answer covers the interval since the last — a status bar asking once
// a second gets a second's worth.
uint32_t fw_cpu_percent(void);

// Signal strength of the network this device is CONNECTED to, in dBm, or 0 when
// there is no reading. Negative: -50 is excellent, -70 fair, -85 poor.
int      fw_net_rssi(void);

// Read from an OFFSET, so a file bigger than anything a package can hold is
// still readable a piece at a time. Returns how many bytes were read: short at
// the end, zero past it. Added at 1.16 because fw_file_read alone capped httpd
// downloads at one buffer.
uint32_t fw_file_read_at(const char *path, uint32_t offset, void *buf, uint32_t cap);

// How big a file is, or 0 if it is absent or a directory. Enough to send a
// Content-Length before the body rather than leaving the far end to guess.
uint32_t fw_file_size(const char *path);

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
// The system clock, in Hz. Anything that derives a rate from it has to ask:
// the default differs between the two chips and the clock is adjustable at
// runtime, so a divider worked out against an assumed frequency is right on
// exactly one board.
uint32_t fw_clock_hz(void);

uint32_t fw_micros(void);
void     fw_busy_wait_us(uint32_t us);

// --- power ------------------------------------------------------------------
//
// Added at 1.12, and unlike everything else here it STOPS THE MACHINE. Every
// task stops, the USB console drops and comes back as a new connection, and the
// system clock does not advance while the device is out — so a task deadline
// set before a sleep does not mean what it did.
//
// Refused rather than obeyed when the request makes no sense: a sleep with
// neither a duration nor a wake pin would never end, which is not a sleep.
//
// `wake_pin` of -1 means "time only". `ms` of 0 with a pin means "until the pin
// changes, however long that takes".
int fw_power_sleep(unsigned ms, int wake_pin, int wake_high);

// Deeper: more clocks stopped, more saved, longer to come back. Same rules.
int fw_power_dormant(unsigned ms, int wake_pin, int wake_high);

// The shortest sleep this board will honour, in milliseconds. Differs by chip —
// two seconds on RP2040, ten milliseconds on RP2350 — so a package that sleeps
// on a schedule has to ask rather than pick a number.
unsigned fw_power_min_sleep_ms(void);

// --- drawing ----------------------------------------------------------------
//
// A 1-bit framebuffer, for the panels a device like this actually carries. The
// TUI layer above is text on a terminal; this is pixels in a buffer, and the
// two do not overlap.
//
// MONO_VLSB — one byte is eight VERTICAL pixels, page-major — which is what the
// SH1106, SSD1306 and SSD1309 all want, so a finished buffer goes to the panel
// as one I2C or SPI write with no repacking.
//
// The package owns the memory. fw_fb_bytes says how much a given size needs;
// where it comes from is the package's business, and that keeps allocation
// lifetime out of the ABI entirely.
struct FwFrameBuf {
    unsigned char *buf;
    int            w;
    int            h;
};

int  fw_fb_bytes(int w, int h);
void fw_fb_fill(struct FwFrameBuf *f, int colour);
void fw_fb_pixel(struct FwFrameBuf *f, int x, int y, int colour);
int  fw_fb_get(const struct FwFrameBuf *f, int x, int y);
void fw_fb_hline(struct FwFrameBuf *f, int x, int y, int len, int colour);
void fw_fb_vline(struct FwFrameBuf *f, int x, int y, int len, int colour);
void fw_fb_line(struct FwFrameBuf *f, int x0, int y0, int x1, int y1, int colour);
void fw_fb_rect(struct FwFrameBuf *f, int x, int y, int w, int h, int colour, int filled);
void fw_fb_text(struct FwFrameBuf *f, const char *s, int x, int y, int colour);
int  fw_fb_text_width(const char *s);
// `transparent` names a colour to skip, or -1 to copy every pixel — the
// difference between stamping an icon over a background and punching a hole.
void fw_fb_blit(struct FwFrameBuf *dst, const struct FwFrameBuf *src,
                int x, int y, int transparent);
void fw_fb_scroll(struct FwFrameBuf *f, int dx, int dy);

// --- PWM --------------------------------------------------------------------
//
// Backlights, buzzers, servos, motor speed. Duty is per MILLE rather than a
// float or a raw count: 0-1000 covers every real use to a tenth of a percent,
// reads the same on both chips, and keeps floating point out of the ABI where
// the calling convention differs between them.
//
// The pin decides the slice, so two pins on the same slice share a frequency —
// that is the hardware, not a limitation of this. fw_pwm_init reports the
// frequency actually achieved, which will differ from the one asked for
// because the divider is not infinitely fine.
int fw_pwm_init(unsigned pin, unsigned freq_hz);     // returns the real frequency
int fw_pwm_duty(unsigned pin, unsigned permille);    // 0-1000
int fw_pwm_stop(unsigned pin);

// --- UART -------------------------------------------------------------------
//
// Both are free: this OS puts its console on USB, so neither UART is spoken for.
// That is what makes the ESP32-C5 link possible without giving up a serial
// terminal.
//
// The read is the interesting one. It waits up to `timeout_ms` for as much as
// was asked for and returns what it got, so a caller can ask for a fixed-length
// frame without hanging if the other end stops talking mid-frame.
int fw_uart_init(unsigned bus, unsigned tx, unsigned rx, unsigned baud);
int fw_uart_write(unsigned bus, const void *data, unsigned len);
int fw_uart_read(unsigned bus, void *buf, unsigned len, unsigned timeout_ms);
int fw_uart_available(unsigned bus);                 // bytes waiting, or -1
int fw_uart_deinit(unsigned bus);

// --- system -----------------------------------------------------------------
//
// Added at 1.10. Every one of these was already in the OS and simply had no
// door: the Nova D1 audit found urandom, hashlib, unique_id, reset and the
// clock all used and none of them reachable.

// Hardware random. Not a PRNG — this is the ring-oscillator entropy source the
// chip provides, so it is fit for a key or a nonce as well as for a game.
unsigned long fw_random(void);
void          fw_random_bytes(void *buf, unsigned len);

// This board's unique identity, as lowercase hex. Sixteen characters plus a
// terminator. Stable across reboots and reflashes — it comes from the flash
// part, not from anything the OS stores.
int fw_unique_id(char *out, unsigned cap);

// SHA-256 of one buffer. `out` takes 32 bytes.
void fw_sha256(const void *data, unsigned len, unsigned char *out);

// Restart the device. Does not return.
void fw_reboot(void);

// The wall clock. Zero-filled and 0 returned when it has never been set, which
// is the normal state on a device that has not seen a time server — so a
// package must check rather than assume the year is sensible.
struct FwTime {
    int year;      // 2026, not 126
    int month;     // 1-12
    int day;       // 1-31
    int hour;      // 0-23
    int minute;
    int second;
    int weekday;   // 0 = Sunday
};
int fw_time_get(struct FwTime *out);

// --- network ----------------------------------------------------------------
//
// Added at 1.9, and it was the largest gap by a wide margin. Measured against
// the v1 Nova D1 sources, `network` appears 78 times — more than every hardware
// call in it put together — and the ABI had nothing at all.
//
// None of this is new capability. The OS already joins networks, resolves
// names, verifies TLS and streams downloads to flash; these are the doors into
// what is already there. A package cannot open a raw socket, and deliberately:
// every call here goes through the same one-owner lock and core pinning the
// shell uses, so a package doing network work cannot break the driver in the
// ways this OS has already been broken once.

#define FW_NET_SSID_MAX  33
#define FW_NET_ADDR_MAX  16

struct FwNetAp {
    char ssid[FW_NET_SSID_MAX];
    int  rssi;
    int  channel;
    int  secured;            // non-zero when it wants a password
};

// Is there a working connection right now? Reads a cached value, so it costs
// nothing and can be asked from anywhere at any time.
int fw_net_connected(void);

// The current network's name and address. Both return the length written, or 0.
int fw_net_ssid(char *out, unsigned cap);
int fw_net_ip(char *out, unsigned cap);

// Scan for access points, strongest first. Returns how many were written, or
// negative if the radio could not be used. Takes seconds.
int fw_net_scan(FwNetAp *out, unsigned max);

// Resolve a hostname. Returns the length written to `ip_out`, or negative.
int fw_net_resolve(const char *host, char *ip_out, unsigned cap);

// Fetch a URL into a buffer. Returns the number of bytes, or negative on
// error. HTTPS is verified against the built-in roots; an unverifiable
// connection is refused rather than downgraded, exactly as it is for the shell.
int fw_http_get(const char *url, void *buf, unsigned cap);

// Fetch a URL straight to a file, which is how anything larger than RAM is
// handled. Returns bytes written, or negative.
int fw_http_download(const char *url, const char *path);

// Fetch a URL and throw it away, reporting how much arrived and how long it
// took. Returns the byte count, or negative.
//
// For measuring a link rather than retrieving anything: the buffer form is
// bounded by the buffer and the file form needs the whole download to fit in
// flash, which a throughput test does not and should not. `ms` times the
// TRANSFER — the clock starts at the first byte, so name lookup and the TLS
// handshake are not counted against the rate — and is never zero when anything
// arrived, so dividing by it is safe.
int fw_http_measure(const char *url, uint32_t *bytes, uint32_t *ms);

// --- directories (API 1.15) -------------------------------------------------
//
// Read BY INDEX rather than through an open/read/close handle, and that is a
// sandbox decision rather than a stylistic one. A handle needs state the
// firmware must clean up when a task dies, and it brings use-after-close and
// double-close with it — every one of which havoc found in the socket API and
// none of which can exist here. A callback would be worse still: calling INTO
// unprivileged package code is the hard direction across this boundary.
//
// The cost is that listing a directory of n entries walks it n times. For the
// sizes involved that is not worth a handle.
//
// The directory must not be changing underneath: an entry added or removed
// between calls shifts the indices after it. For a listing that matters, read
// fw_dir_count first and stop if it changes.

#define FW_NAME_MAX 64

typedef struct {
    char     name[FW_NAME_MAX];
    int      is_dir;
    uint32_t size;              // bytes; meaningless for a directory
} FwDirEntry;

// How many entries the directory has, or negative if it cannot be read.
int fw_dir_count(const char *path);

// One entry. Returns 1 when `out` was filled, 0 when `index` is past the end,
// negative when the directory cannot be read.
int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out);

// --- TCP (API 1.14) ---------------------------------------------------------
//
// The five calls a server is built from. httpd is a package because of these,
// and a LAN scanner or the Nova D1's networking wants the same five.
//
// A handle is a small opaque integer, never a pointer: the firmware looks it up
// in a table it owns, so the worst a bad one can do is return -1. It carries a
// generation too, so a handle kept after a close does not quietly land on
// whichever socket got the slot next.
//
// Errors are -1 for "wrong, and it will stay wrong" and -2 for "timed out or
// interrupted, try again". A package that cannot tell those apart will either
// spin on a dead socket or give up on a slow one.

// Bind and listen. Returns a handle, or -1 if the port is taken or there is no
// room. Nothing is accepted until fw_tcp_accept asks.
int fw_tcp_listen(unsigned port);

// Take the next waiting connection. Returns a connection handle, -2 if none
// arrived within the timeout (including Ctrl+C), -1 if the listener is invalid.
// Yields while it waits, so other tasks keep running.
int fw_tcp_accept(int listener, uint32_t timeout_ms);

// Read what has arrived. Returns the byte count, 0 when the peer has closed and
// there is nothing left, -2 on timeout, -1 on a broken connection.
//
// Bytes are acknowledged as they are READ, not as they arrive, so a slow reader
// slows the sender down instead of losing data.
int fw_tcp_recv(int conn, void *buf, unsigned cap, uint32_t timeout_ms);

// Write. Returns how many bytes were accepted, which may be fewer than asked
// for on a slow link. The data is copied, so the buffer is free immediately.
int fw_tcp_send(int conn, const void *buf, unsigned len);

// Close a connection or a listener. A socket left open when its task ends is
// closed for it, but a long-running package should not rely on that.
int fw_tcp_close(int handle);

// Send one ICMP echo and time the reply. Returns the round trip in
// MICROSECONDS, or negative: -1 could not send, -2 no reply in time.
//
// Real ICMP, the same as the `ping` command. v1's packages timed a TCP connect
// instead and said why in a comment — MicroPython could not send an echo
// request. This is the measurement itself rather than a stand-in for it.
int fw_net_ping(const char *host, uint32_t timeout_ms);

// --- SPI --------------------------------------------------------------------
//
// Added at 1.8 because every radio and storage part worth supporting is on it:
// CC1101, SX1276, an SD card, the ESP32-C5 link. The ABI had GPIO, ADC, I2C and
// PIO and not the bus most of the hardware actually uses.
//
// Chip select is deliberately NOT handled here. Parts disagree about when it
// may be released, several need it held across a run of transfers, and some
// want a delay either side — so it stays an ordinary GPIO the package drives,
// which is both simpler and more capable than any wrapper would be.
int fw_spi_init(unsigned bus, unsigned sck, unsigned mosi, unsigned miso, unsigned baud);
int fw_spi_set_baud(unsigned bus, unsigned baud);      // returns the rate actually set
int fw_spi_write(unsigned bus, const void *data, unsigned len);
int fw_spi_read(unsigned bus, void *buf, unsigned len, unsigned char tx_fill);
int fw_spi_transfer(unsigned bus, const void *tx, void *rx, unsigned len);
int fw_spi_deinit(unsigned bus);

// --- PIO --------------------------------------------------------------------
//
// Added at 1.7, and the reason is timing. fw_busy_wait_us measures accurate to
// about a microsecond on a quiet device, which is fine for anything with tens
// of microseconds of tolerance and nowhere near enough for WS2812 (150 ns) or a
// fast display. The difference is not precision of the loop, it is that a CPU
// loop can be interrupted and a state machine cannot.
//
// A PIO program runs on its own hardware at up to the system clock, cycle
// exact, and carries on regardless of what the scheduler, the USB stack or the
// radio are doing. That is the only way this OS can promise steady output while
// it is also doing everything else.
//
// Programs arrive as ASSEMBLED 16-bit words. There is no assembler on the
// device — the encoders below build the words, so a package's program is
// readable in source and costs nothing at runtime.

#define FW_PIO_MAX_PROGRAM 32

// Shift direction for the input and output registers.
#define FW_PIO_SHIFT_LEFT   0
#define FW_PIO_SHIFT_RIGHT  1

// Claim a state machine. Returns a handle, or -1 when they are all in use —
// there are 8 on RP2040 and 12 on RP2350, shared between every package, so a
// package that takes one and never gives it back starves the rest.
int  fw_pio_claim(void);
void fw_pio_release(int h);

// Load `len` assembled words. `wrap` and `wrap_target` are instruction offsets:
// execution runs to `wrap` then jumps back to `wrap_target`, which is how a PIO
// program loops without spending an instruction on a jump.
int  fw_pio_load(int h, const unsigned short *prog, unsigned len,
                 unsigned wrap_target, unsigned wrap);

// `clk_div_x256` is the divider in 24.8 fixed point: 256 is full speed, 512 is
// half. Fixed point rather than a float so the ABI carries no floating-point
// calling convention, which differs between the two chips.
int  fw_pio_config_pins(int h, unsigned out_base, unsigned out_count,
                        unsigned set_base, unsigned set_count,
                        unsigned sideset_base, unsigned sideset_count);
int  fw_pio_config_shift(int h, int out_shift_dir, int autopull, unsigned pull_threshold);
int  fw_pio_config_clock(int h, unsigned clk_div_x256);

int  fw_pio_start(int h);
void fw_pio_stop(int h);

// FIFO. put returns 0 when the word was accepted, -1 if the queue stayed full
// for `timeout_us`. get returns 1 on a word, 0 on nothing, -1 on a bad handle.
int  fw_pio_put(int h, unsigned long value, unsigned timeout_us);
int  fw_pio_get(int h, unsigned long *out, unsigned timeout_us);

// How many state machines exist, and how many are still free.
unsigned fw_pio_count(void);
unsigned fw_pio_free(void);

// --- instruction encoders ---------------------------------------------------
//
// Header-only, so they cost no ABI surface and a package can build its program
// at compile time. `delay` is the cycles to idle after the instruction, which
// is where most of a PIO program's timing actually lives.

#define FW_PIO_DELAY(d)          (((d) & 0x1f) << 8)

// SET dst, value  — dst: 0 pins, 1 x, 2 y, 4 pindirs
#define FW_PIO_SET(dst, val, delay) \
    ((unsigned short)(0xe000u | FW_PIO_DELAY(delay) | (((dst) & 7) << 5) | ((val) & 0x1f)))

// JMP cond, addr  — cond: 0 always, 1 !x, 2 x--, 3 !y, 4 y--, 5 x!=y, 6 pin, 7 !osre
#define FW_PIO_JMP(cond, addr, delay) \
    ((unsigned short)(0x0000u | FW_PIO_DELAY(delay) | (((cond) & 7) << 5) | ((addr) & 0x1f)))

// OUT dst, count  — dst: 0 pins, 1 x, 2 y, 3 null, 4 pindirs, 5 pc, 6 isr, 7 exec
#define FW_PIO_OUT(dst, count, delay) \
    ((unsigned short)(0x6000u | FW_PIO_DELAY(delay) | (((dst) & 7) << 5) | ((count) & 0x1f)))

// IN src, count   — src: 0 pins, 1 x, 2 y, 3 null, 6 isr, 7 osr
#define FW_PIO_IN(src, count, delay) \
    ((unsigned short)(0x4000u | FW_PIO_DELAY(delay) | (((src) & 7) << 5) | ((count) & 0x1f)))

// NOP is MOV y, y — the idiom PIO uses, since it has no dedicated encoding.
#define FW_PIO_NOP(delay) \
    ((unsigned short)(0xa000u | FW_PIO_DELAY(delay) | (2 << 5) | 2))

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
