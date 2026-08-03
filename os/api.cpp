// The exported symbol table — the OS side of the app ABI.
//
// A flat array with a linear scan: lookup happens a handful of times per app at
// load time, so a sorted table and a binary search would trade flash for a
// saving nobody can measure. Every entry is a permanent commitment; adding one
// is a MINOR bump, changing one is MAJOR.

#include "api.h"
#include "rpc_app.h"
#include "kernel.h"
#include "command.h"
#include "task.h"
#include "storage.h"
#include "logring.h"
#include "blackbox.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"

// The app currently being run, so a command it registers can be tagged with an
// owner and swept when the app unloads. Set by the shell around app_main. Same
// pattern as the fault handler's g_current_app.
static void *g_current_owner = nullptr;
extern "C" void api_set_current_app(void *owner) { g_current_owner = owner; }

// --- implementations -------------------------------------------------------

// Every one of these calls task_alive first.
//
// A package doing real work calls into the ABI constantly — printing, timing,
// allocating, touching files — so this is the liveness signal, and it costs the
// package nothing to provide. Without it, code that legitimately runs for a
// while without yielding is indistinguishable from a hang, and gets rebooted for
// doing its job.
extern "C" int fw_printf(const char *fmt, ...) {
    // Reaching here at all proves the call from package code into the firmware
    // works — the veneer, the relocation and the symbol lookup. If a crash
    // report stops at "calling package 'x'" and never shows this, the fault is
    // in getting here, not in anything the package does.
    bb_note_phase("entered fw_printf");
    task_alive();
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap); va_end(ap);
    return n;
}
extern "C" uint32_t fw_millis(void) { bb_note_phase("entered fw_millis"); task_alive(); return (uint32_t)(time_us_64() / 1000u); }
extern "C" void    *fw_malloc(size_t n) { bb_note_phase("entered fw_malloc"); task_alive(); return malloc(n); }
extern "C" void     fw_free(void *p)    { task_alive(); free(p); }

extern "C" void fw_log(int level, const char *msg) {
    LogLevel l = (level == 2) ? LOG_ERROR : (level == 1) ? LOG_WARN : LOG_INFO;
    klog(l, "%s", msg ? msg : "");
}

extern "C" int rpc_register_command(const char *name, const char *help,
                                    RpcCommandFn fn) {
    Command c{name, help ? help : "", (CommandFn)fn, g_current_owner};
    return cmd_register(&c) ? 1 : 0;
}

// --- API 1.3: tasks, files, memory ------------------------------------------
//
// Without these a package can print and allocate and nothing else, which is not
// enough to be a program. They are the smallest set that lets one run work in
// the background, keep state on disk, and see what memory it is using — added
// as a MINOR bump, so every existing package still loads.

extern "C" int fw_task_spawn(const char *name, TaskFn fn, void *arg, uint32_t stack) {
    // A package's task is AFFINITY_ANY, so it uses the second core when there is
    // one and the first when there is not. A package should never have to know.
    return task_spawn(name, "(package)", fn, arg,
                      stack ? stack : TASK_STACK_DEF, AFFINITY_ANY);
}
extern "C" void fw_task_yield(void)            { task_yield(); }
extern "C" void fw_task_sleep_ms(uint32_t ms)  { task_sleep_ms(ms); }
extern "C" int  fw_task_self(void)             { return task_self(); }
extern "C" int  fw_task_should_stop(void)      { return task_should_stop() ? 1 : 0; }
extern "C" int  fw_task_kill(int pid)          { return task_kill(pid) ? 1 : 0; }
extern "C" uint32_t fw_cores(void)             { return task_core_count(); }

extern "C" int fw_file_write(const char *path, const void *data, uint32_t len) {
    task_alive();
    return storage_write_file(path, (const uint8_t *)data, len) ? 1 : 0;
}
extern "C" uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    task_alive();
    return storage_read_file(path, (uint8_t *)buf, cap);
}
extern "C" int fw_file_remove(const char *path) { task_alive(); return storage_remove(path) ? 1 : 0; }
extern "C" int fw_file_exists(const char *path) { return storage_stat(path, nullptr, nullptr) ? 1 : 0; }

// --- hardware ---------------------------------------------------------------
//
// Thin wrappers over the SDK, with the pin checked first. On RP2 an out of
// range pin number is not an error the hardware reports — it aliases onto some
// other register — so refusing here is the difference between "that pin does
// not exist" and a device that behaves strangely for reasons nobody can find.

// Pins the OS owns, which a package must not have. On the wireless boards the
// radio is wired to GPIO 23/24/25/29 through the CYW43, and driving those from
// a package takes the network down in a way that looks like a WiFi fault.
static bool pin_reserved(unsigned pin) {
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
    return pin == 23 || pin == 24 || pin == 25 || pin == 29;
#else
    (void)pin;
    return false;
#endif
}

extern "C" uint32_t fw_core_id(void) { return task_this_core(); }

extern "C" unsigned fw_gpio_count(void) { return NUM_BANK0_GPIOS; }

extern "C" int fw_gpio_usable(unsigned pin) {
    return (pin < NUM_BANK0_GPIOS && !pin_reserved(pin)) ? 1 : 0;
}

extern "C" int fw_gpio_init(unsigned pin, int dir) {
    if (!fw_gpio_usable(pin)) return -1;
    task_alive();
    gpio_init(pin);
    gpio_set_dir(pin, dir == FW_PIN_OUT);
    return 0;
}

extern "C" int fw_gpio_pull(unsigned pin, int mode) {
    if (!fw_gpio_usable(pin)) return -1;
    gpio_set_pulls(pin, mode == FW_PULL_UP, mode == FW_PULL_DOWN);
    return 0;
}

extern "C" int fw_gpio_put(unsigned pin, int value) {
    if (!fw_gpio_usable(pin)) return -1;
    gpio_put(pin, value != 0);
    return 0;
}

extern "C" int fw_gpio_get(unsigned pin) {
    if (!fw_gpio_usable(pin)) return -1;
    return gpio_get(pin) ? 1 : 0;
}

// The ADC. Channel 4 is the temperature sensor on RP2040; RP2350 moved it to
// channel 4 as well on the 30-pin parts but the SDK's own constant is the only
// thing worth trusting here, so it is reported rather than assumed.
static bool g_adc_ready;

extern "C" unsigned fw_adc_temp_channel(void) { return 4; }

extern "C" int fw_adc_init(unsigned channel) {
    if (channel > 4) return -1;
    if (!g_adc_ready) { adc_init(); g_adc_ready = true; }
    if (channel == fw_adc_temp_channel()) adc_set_temp_sensor_enabled(true);
    else                                  adc_gpio_init(26 + channel);
    return 0;
}

extern "C" int fw_adc_read(unsigned channel) {
    if (channel > 4 || !g_adc_ready) return -1;
    task_alive();
    adc_select_input(channel);
    return (int)adc_read();
}

// I2C. Returning the SDK's byte count straight through is what makes a bus scan
// work: a zero-length write to an address either acknowledges or does not, and
// the caller can tell those apart without a separate probe call.
static i2c_inst_t *i2c_of(unsigned bus) {
    if (bus == 0) return i2c0;
    if (bus == 1) return i2c1;
    return nullptr;
}
static bool g_i2c_ready[2];

extern "C" int fw_i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned baud) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i) return -1;
    if (!fw_gpio_usable(sda) || !fw_gpio_usable(scl)) return -1;
    if (baud == 0) baud = 100000;
    task_alive();
    i2c_init(i, baud);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);
    g_i2c_ready[bus] = true;
    return 0;
}

extern "C" int fw_i2c_write(unsigned bus, unsigned addr, const void *data,
                            unsigned len, int nostop) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus] || addr > 0x7f) return -1;
    task_alive();
    return i2c_write_blocking(i, (uint8_t)addr, (const uint8_t *)data, len, nostop != 0);
}

extern "C" int fw_i2c_read(unsigned bus, unsigned addr, void *buf,
                           unsigned len, int nostop) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus] || addr > 0x7f) return -1;
    task_alive();
    return i2c_read_blocking(i, (uint8_t)addr, (uint8_t *)buf, len, nostop != 0);
}

extern "C" int fw_i2c_deinit(unsigned bus) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus]) return -1;
    i2c_deinit(i);
    g_i2c_ready[bus] = false;
    return 0;
}

extern "C" uint32_t fw_clock_hz(void) { return clock_get_hz(clk_sys); }

extern "C" uint32_t fw_micros(void) { return time_us_32(); }

// Busy, not yielding, and that is the point. A protocol timed in microseconds
// cannot survive a task switch in the middle of it — a DHT's whole conversation
// is over in about 5 ms, and yielding once loses the reading. Capped so this
// cannot be used to take the core away indefinitely; anything longer belongs in
// fw_task_sleep_ms, which yields properly.
extern "C" void fw_busy_wait_us(uint32_t us) {
    if (us > 20000) us = 20000;
    busy_wait_us(us);
    task_alive();
}

// --- SPI --------------------------------------------------------------------

static spi_inst_t *spi_of(unsigned bus) {
    if (bus == 0) return spi0;
    if (bus == 1) return spi1;
    return nullptr;
}
static bool g_spi_ready[2];

extern "C" int fw_spi_init(unsigned bus, unsigned sck, unsigned mosi,
                           unsigned miso, unsigned baud) {
    spi_inst_t *s = spi_of(bus);
    if (!s) return -1;
    if (!fw_gpio_usable(sck) || !fw_gpio_usable(mosi)) return -1;
    // miso is optional: a write-only device (a display, a shift register) has
    // nothing to send back, and demanding a pin for it would waste one.
    if (miso != 0xffffffffu && !fw_gpio_usable(miso)) return -1;
    if (baud == 0) baud = 1000000;

    task_alive();
    int actual = (int)spi_init(s, baud);
    gpio_set_function(sck,  GPIO_FUNC_SPI);
    gpio_set_function(mosi, GPIO_FUNC_SPI);
    if (miso != 0xffffffffu) gpio_set_function(miso, GPIO_FUNC_SPI);
    g_spi_ready[bus] = true;
    return actual;
}

extern "C" int fw_spi_set_baud(unsigned bus, unsigned baud) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || baud == 0) return -1;
    return (int)spi_set_baudrate(s, baud);
}

extern "C" int fw_spi_write(unsigned bus, const void *data, unsigned len) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !data) return -1;
    task_alive();
    return spi_write_blocking(s, (const uint8_t *)data, len);
}

extern "C" int fw_spi_read(unsigned bus, void *buf, unsigned len, unsigned char tx_fill) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !buf) return -1;
    task_alive();
    return spi_read_blocking(s, tx_fill, (uint8_t *)buf, len);
}

extern "C" int fw_spi_transfer(unsigned bus, const void *tx, void *rx, unsigned len) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !tx || !rx) return -1;
    task_alive();
    return spi_write_read_blocking(s, (const uint8_t *)tx, (uint8_t *)rx, len);
}

extern "C" int fw_spi_deinit(unsigned bus) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus]) return -1;
    spi_deinit(s);
    g_spi_ready[bus] = false;
    return 0;
}

// --- PIO --------------------------------------------------------------------
//
// State machines are a fixed, shared resource — 8 on RP2040, 12 on RP2350 —
// so they are handed out through a claim table rather than by letting packages
// pick. Two packages both deciding to use PIO0 SM0 is not something either of
// them could detect.

#if PICO_RP2040
#define PIO_BLOCKS 2
#else
#define PIO_BLOCKS 3
#endif
#define PIO_SMS_PER_BLOCK 4
#define PIO_SLOTS (PIO_BLOCKS * PIO_SMS_PER_BLOCK)

struct PioSlot {
    bool     used;
    PIO      pio;
    uint     sm;
    int      offset;        // where the program was loaded, -1 if none
    uint8_t  prog_len;      // needed to give the instruction memory back
    pio_sm_config cfg;
};
static PioSlot g_pio[PIO_SLOTS];

static PIO pio_block(int i) {
    switch (i) {
        case 0: return pio0;
        case 1: return pio1;
#if !PICO_RP2040
        case 2: return pio2;
#endif
        default: return pio0;
    }
}

static PioSlot *pio_slot(int h) {
    if (h < 0 || h >= PIO_SLOTS || !g_pio[h].used) return nullptr;
    return &g_pio[h];
}

extern "C" unsigned fw_pio_count(void) { return PIO_SLOTS; }

extern "C" unsigned fw_pio_free(void) {
    unsigned n = 0;
    for (int i = 0; i < PIO_SLOTS; i++) if (!g_pio[i].used) n++;
    return n;
}

extern "C" int fw_pio_claim(void) {
    for (int i = 0; i < PIO_SLOTS; i++) {
        if (g_pio[i].used) continue;
        PIO p = pio_block(i / PIO_SMS_PER_BLOCK);
        uint sm = (uint)(i % PIO_SMS_PER_BLOCK);
        // Ask the SDK rather than assume: something else in the firmware may
        // already hold this one, and claiming it twice is silent breakage.
        if (pio_sm_is_claimed(p, sm)) continue;
        pio_sm_claim(p, sm);
        g_pio[i].used   = true;
        g_pio[i].pio    = p;
        g_pio[i].sm     = sm;
        g_pio[i].offset = -1;
        g_pio[i].cfg    = pio_get_default_sm_config();
        return i;
    }
    return -1;
}

extern "C" void fw_pio_release(int h) {
    PioSlot *s = pio_slot(h);
    if (!s) return;
    pio_sm_set_enabled(s->pio, s->sm, false);

    // Give the instruction memory back, not just the state machine.
    //
    // There are 32 instruction slots per PIO block and they are the scarcer
    // resource — four state machines share them. Releasing the machine without
    // removing the program leaked those slots, so a package claiming and
    // releasing in a loop would eventually fail to load anything with the state
    // machines all reporting free. The length is kept for exactly this.
    if (s->offset >= 0 && s->prog_len) {
        pio_program pp;
        pp.instructions = nullptr;      // remove only needs origin and length
        pp.length       = s->prog_len;
        pp.origin       = (int8_t)s->offset;
#if !PICO_RP2040
        pp.pio_version  = 0;
        pp.used_gpio_ranges = 0;
#endif
        pio_remove_program(s->pio, &pp, (uint)s->offset);
    }

    pio_sm_unclaim(s->pio, s->sm);
    s->used = false;
    s->offset = -1;
    s->prog_len = 0;
}

extern "C" int fw_pio_load(int h, const unsigned short *prog, unsigned len,
                           unsigned wrap_target, unsigned wrap) {
    PioSlot *s = pio_slot(h);
    if (!s || !prog || len == 0 || len > 32) return -1;
    if (wrap >= len || wrap_target >= len) return -1;

    pio_program pp;
    pp.instructions = prog;
    pp.length       = (uint8_t)len;
    pp.origin       = -1;                 // let the SDK place it
#if !PICO_RP2040
    pp.pio_version  = 0;
    pp.used_gpio_ranges = 0;
#endif
    if (!pio_can_add_program(s->pio, &pp)) return -1;
    s->offset   = pio_add_program(s->pio, &pp);
    s->prog_len = (uint8_t)len;
    sm_config_set_wrap(&s->cfg, (uint)(s->offset + wrap_target), (uint)(s->offset + wrap));
    return s->offset;
}

extern "C" int fw_pio_config_pins(int h, unsigned out_base, unsigned out_count,
                                  unsigned set_base, unsigned set_count,
                                  unsigned sideset_base, unsigned sideset_count) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    // Every pin validated the same way the GPIO calls are, and for the same
    // reason: a PIO program driving a pin the radio owns takes the network down.
    if (out_count   && !fw_gpio_usable(out_base))     return -1;
    if (set_count   && !fw_gpio_usable(set_base))     return -1;
    if (sideset_count && !fw_gpio_usable(sideset_base)) return -1;

    if (out_count) {
        for (unsigned i = 0; i < out_count; i++) {
            if (!fw_gpio_usable(out_base + i)) return -1;
            pio_gpio_init(s->pio, out_base + i);
        }
        sm_config_set_out_pins(&s->cfg, out_base, out_count);
        pio_sm_set_consecutive_pindirs(s->pio, s->sm, out_base, out_count, true);
    }
    if (set_count) {
        for (unsigned i = 0; i < set_count; i++) {
            if (!fw_gpio_usable(set_base + i)) return -1;
            pio_gpio_init(s->pio, set_base + i);
        }
        sm_config_set_set_pins(&s->cfg, set_base, set_count);
        pio_sm_set_consecutive_pindirs(s->pio, s->sm, set_base, set_count, true);
    }
    if (sideset_count) {
        for (unsigned i = 0; i < sideset_count; i++) {
            if (!fw_gpio_usable(sideset_base + i)) return -1;
            pio_gpio_init(s->pio, sideset_base + i);
        }
        sm_config_set_sideset_pins(&s->cfg, sideset_base);
    }
    return 0;
}

extern "C" int fw_pio_config_shift(int h, int out_shift_dir, int autopull,
                                   unsigned pull_threshold) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    if (pull_threshold > 32) return -1;
    sm_config_set_out_shift(&s->cfg, out_shift_dir == FW_PIO_SHIFT_RIGHT,
                            autopull != 0, pull_threshold ? pull_threshold : 32);
    return 0;
}

extern "C" int fw_pio_config_clock(int h, unsigned clk_div_x256) {
    PioSlot *s = pio_slot(h);
    if (!s || clk_div_x256 == 0) return -1;
    // 24.8 fixed point, split into the integer and fractional halves the
    // hardware wants. No float crosses the ABI, which matters because the
    // calling convention for one is not the same on both chips.
    uint16_t whole = (uint16_t)(clk_div_x256 >> 8);
    uint8_t  frac  = (uint8_t)(clk_div_x256 & 0xff);
    if (whole == 0 && frac == 0) return -1;
    sm_config_set_clkdiv_int_frac(&s->cfg, whole, frac);
    return 0;
}

extern "C" int fw_pio_start(int h) {
    PioSlot *s = pio_slot(h);
    if (!s || s->offset < 0) return -1;
    pio_sm_init(s->pio, s->sm, (uint)s->offset, &s->cfg);
    pio_sm_set_enabled(s->pio, s->sm, true);
    return 0;
}

extern "C" void fw_pio_stop(int h) {
    PioSlot *s = pio_slot(h);
    if (s) pio_sm_set_enabled(s->pio, s->sm, false);
}

extern "C" int fw_pio_put(int h, unsigned long value, unsigned timeout_us) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    uint32_t start = time_us_32();
    while (pio_sm_is_tx_fifo_full(s->pio, s->sm)) {
        if (time_us_32() - start > timeout_us) return -1;
        // Yield only when the wait is long enough to be worth it. A FIFO with
        // four slots drains in microseconds at any sensible clock, and yielding
        // on every word would cost more than the transfer.
        if (time_us_32() - start > 200) task_yield();
    }
    pio_sm_put(s->pio, s->sm, (uint32_t)value);
    return 0;
}

extern "C" int fw_pio_get(int h, unsigned long *out, unsigned timeout_us) {
    PioSlot *s = pio_slot(h);
    if (!s || !out) return -1;
    uint32_t start = time_us_32();
    while (pio_sm_is_rx_fifo_empty(s->pio, s->sm)) {
        if (time_us_32() - start > timeout_us) return 0;
        if (time_us_32() - start > 200) task_yield();
    }
    *out = pio_sm_get(s->pio, s->sm);
    return 1;
}

extern "C" uint32_t fw_heap_free(void)  { return heap_free(); }
extern "C" uint32_t fw_heap_total(void) { return heap_total(); }

// A checkpoint that survives a hang. Printed output does not: whatever is in the
// USB buffer when the device stops is never delivered, which is why a crash
// report can name the command but not the line. This is recorded in memory the
// reset does not clear, so the last checkpoint reached IS the failing step.
extern "C" void fw_progress(const char *what) { task_alive(); bb_note_phase(what); }

// The biggest single allocation available right now, found by probing. Free
// bytes do not predict whether the next allocation succeeds; this does.
extern "C" uint32_t fw_heap_largest(void) {
    uint32_t lo = 0, hi = heap_free(), best = 0;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (mid == 0) break;
        void *p = malloc(mid);
        if (p) { free(p); best = mid; lo = mid + 1024; }
        else   { if (mid < 1024) break; hi = mid - 1024; }
    }
    return best;
}

// --- the compiler's runtime -------------------------------------------------
//
// Not "API" in the versioned sense — these are the helpers GCC EMITS CALLS TO
// without being asked. An integer % becomes __aeabi_idivmod, a struct copy
// becomes memcpy, and neither appears anywhere in the package's source. Leaving
// them out meant any package doing arithmetic more complicated than addition
// failed to load with "unresolved symbol", which is exactly what happened to the
// self test.
//
// They are listed separately from the API because they are not a compatibility
// commitment this OS makes — they are the C runtime the compiler assumes exists,
// and adding one is not a MINOR bump.
extern "C" {
// Integer division. Cortex-M0+ has no divide instruction at all, so on RP2040
// even a plain / turns into one of these.
void __aeabi_idiv(void);
void __aeabi_idivmod(void);
void __aeabi_uidiv(void);
void __aeabi_uidivmod(void);
void __aeabi_ldivmod(void);
void __aeabi_uldivmod(void);
// 64-bit arithmetic.
void __aeabi_lmul(void);
void __aeabi_llsl(void);
void __aeabi_llsr(void);
void __aeabi_lasr(void);
// Double-precision soft float. Neither RP2040 nor RP2350 has a double FPU, so
// every `double` operation in a package is one of these — including comparisons
// and the conversions at the edges. The first converted package that did any
// arithmetic at all (calc) needed eleven of them, which is a fair sign that
// leaving them out would have met every later one too.
void __aeabi_dadd(void);
void __aeabi_dsub(void);
void __aeabi_dmul(void);
void __aeabi_ddiv(void);
void __aeabi_dcmpeq(void);
void __aeabi_dcmplt(void);
void __aeabi_dcmple(void);
void __aeabi_dcmpge(void);
void __aeabi_dcmpgt(void);
void __aeabi_dcmpun(void);
void __aeabi_d2iz(void);
void __aeabi_d2uiz(void);
void __aeabi_d2lz(void);
void __aeabi_d2ulz(void);
void __aeabi_i2d(void);
void __aeabi_ui2d(void);
void __aeabi_l2d(void);
void __aeabi_ul2d(void);
// Single precision, for the same reason — a package using float rather than
// double would otherwise hit the identical wall one conversion later.
void __aeabi_fadd(void);
void __aeabi_fsub(void);
void __aeabi_fmul(void);
void __aeabi_fdiv(void);
void __aeabi_fcmpeq(void);
void __aeabi_fcmplt(void);
void __aeabi_fcmple(void);
void __aeabi_fcmpge(void);
void __aeabi_fcmpgt(void);
void __aeabi_f2iz(void);
void __aeabi_i2f(void);
void __aeabi_f2d(void);
void __aeabi_d2f(void);
}

// --- the table -------------------------------------------------------------

struct ApiSymbol { const char *name; uint32_t addr; };
#define SYM(fn) { #fn, (uint32_t)(uintptr_t)(void *)&fn }

static const ApiSymbol kSymbols[] = {
    SYM(fw_printf),
    SYM(fw_millis),
    SYM(fw_malloc),
    SYM(fw_free),
    SYM(fw_log),
    SYM(rpc_register_command),
    // API 1.3
    SYM(fw_task_spawn),
    SYM(fw_task_yield),
    SYM(fw_task_sleep_ms),
    SYM(fw_task_self),
    SYM(fw_task_should_stop),
    SYM(fw_task_kill),
    SYM(fw_cores),
    SYM(fw_file_write),
    SYM(fw_file_read),
    SYM(fw_file_remove),
    SYM(fw_file_exists),
    SYM(fw_core_id),
    SYM(fw_spi_init),
    SYM(fw_spi_set_baud),
    SYM(fw_spi_write),
    SYM(fw_spi_read),
    SYM(fw_spi_transfer),
    SYM(fw_spi_deinit),
    SYM(fw_pio_claim),
    SYM(fw_pio_release),
    SYM(fw_pio_load),
    SYM(fw_pio_config_pins),
    SYM(fw_pio_config_shift),
    SYM(fw_pio_config_clock),
    SYM(fw_pio_start),
    SYM(fw_pio_stop),
    SYM(fw_pio_put),
    SYM(fw_pio_get),
    SYM(fw_pio_count),
    SYM(fw_pio_free),
    SYM(fw_gpio_count),
    SYM(fw_gpio_usable),
    SYM(fw_gpio_init),
    SYM(fw_gpio_pull),
    SYM(fw_gpio_put),
    SYM(fw_gpio_get),
    SYM(fw_adc_init),
    SYM(fw_adc_read),
    SYM(fw_adc_temp_channel),
    SYM(fw_i2c_init),
    SYM(fw_i2c_write),
    SYM(fw_i2c_read),
    SYM(fw_i2c_deinit),
    SYM(fw_clock_hz),
    SYM(fw_micros),
    SYM(fw_busy_wait_us),
    SYM(fw_heap_free),
    SYM(fw_heap_total),
    SYM(fw_heap_largest),
    SYM(fw_progress),
    // API 1.4 — the TUI.
    SYM(fw_tui_begin),
    SYM(fw_tui_end),
    SYM(fw_tui_size),
    SYM(fw_tui_clear),
    SYM(fw_tui_text),
    SYM(fw_tui_box),
    SYM(fw_tui_fill),
    SYM(fw_tui_present),
    SYM(fw_tui_poll),
    SYM(fw_tui_refresh),

    // The compiler's runtime. See above: emitted, not written.
    SYM(__aeabi_idiv),
    SYM(__aeabi_idivmod),
    SYM(__aeabi_uidiv),
    SYM(__aeabi_uidivmod),
    SYM(__aeabi_ldivmod),
    SYM(__aeabi_uldivmod),
    SYM(__aeabi_lmul),
    SYM(__aeabi_llsl),
    SYM(__aeabi_llsr),
    SYM(__aeabi_lasr),
    SYM(__aeabi_dadd),
    SYM(__aeabi_dsub),
    SYM(__aeabi_dmul),
    SYM(__aeabi_ddiv),
    SYM(__aeabi_dcmpeq),
    SYM(__aeabi_dcmplt),
    SYM(__aeabi_dcmple),
    SYM(__aeabi_dcmpge),
    SYM(__aeabi_dcmpgt),
    SYM(__aeabi_dcmpun),
    SYM(__aeabi_d2iz),
    SYM(__aeabi_d2uiz),
    SYM(__aeabi_d2lz),
    SYM(__aeabi_d2ulz),
    SYM(__aeabi_i2d),
    SYM(__aeabi_ui2d),
    SYM(__aeabi_l2d),
    SYM(__aeabi_ul2d),
    SYM(__aeabi_fadd),
    SYM(__aeabi_fsub),
    SYM(__aeabi_fmul),
    SYM(__aeabi_fdiv),
    SYM(__aeabi_fcmpeq),
    SYM(__aeabi_fcmplt),
    SYM(__aeabi_fcmple),
    SYM(__aeabi_fcmpge),
    SYM(__aeabi_fcmpgt),
    SYM(__aeabi_f2iz),
    SYM(__aeabi_i2f),
    SYM(__aeabi_f2d),
    SYM(__aeabi_d2f),
    SYM(memcpy),
    SYM(memset),
    SYM(memmove),
    SYM(memcmp),
    SYM(strlen),
    SYM(strcmp),
    SYM(strncmp),
    SYM(strcpy),
    SYM(strncpy),
    SYM(strchr),
    SYM(strstr),
    SYM(snprintf),
};
static const uint32_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

uint32_t api_lookup(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return kSymbols[i].addr;
    return 0;
}
uint32_t api_symbol_count(void) { return kSymbolCount; }

// --- the TUI (API 1.4) ------------------------------------------------------
//
// A package gets the same drawing surface the built-in apps use. The grid lives
// here rather than in the package so the diffing renderer has something stable
// to compare against, and so a package that crashes mid-draw cannot leave the
// terminal in a state nothing can recover.

#include "tui.h"
#include "tuiterm.h"

// Allocated while a full-screen app runs, not for the whole uptime. ~12 KB is
// worth having back on a device with 374 KB of usable heap, and a session that
// never opens a TUI should not pay for one.
static TuiScreen *g_app_screen;

extern "C" void fw_tui_begin(void) {
    task_alive();
    if (!g_app_screen) g_app_screen = (TuiScreen *)malloc(sizeof(TuiScreen));
    if (!g_app_screen) { fw_log(2, "not enough memory for a full-screen app"); return; }

    // begin FIRST: it is what asks the terminal how big it is, so reading the
    // size before it runs would only ever return the default.
    tuiterm_begin();
    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(g_app_screen, tw, th);
}

extern "C" void fw_tui_end(void) {
    task_alive();
    tuiterm_end();
    free(g_app_screen);
    g_app_screen = nullptr;
}

extern "C" void fw_tui_size(int *w, int *h) {
    if (w) *w = g_app_screen ? g_app_screen->w : 0;
    if (h) *h = g_app_screen ? g_app_screen->h : 0;
}

extern "C" void fw_tui_clear(void) { task_alive(); if (g_app_screen) tui_clear(g_app_screen); }

extern "C" void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg) {
    task_alive();
    if (g_app_screen) tui_text(g_app_screen, x, y, s, attr, fg);
}

extern "C" void fw_tui_box(int x, int y, int w, int h, const char *title,
                           unsigned char attr, unsigned char fg) {
    task_alive();
    if (g_app_screen) tui_box(g_app_screen, x, y, w, h, title, attr, fg);
}

extern "C" void fw_tui_fill(int x, int y, int w, int h, char ch,
                            unsigned char attr, unsigned char fg) {
    task_alive();
    if (g_app_screen) tui_fill(g_app_screen, x, y, w, h, ch, attr, fg);
}

extern "C" void fw_tui_present(void) { task_alive(); if (g_app_screen) tuiterm_present(g_app_screen); }

extern "C" int fw_tui_refresh(void) {
    task_alive();
    if (!g_app_screen) return 0;
    bool changed = tuiterm_refresh();
    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(g_app_screen, tw, th);
    return changed ? 1 : 0;
}

extern "C" int fw_tui_poll(FwTuiEvent *out) {
    task_alive();
    if (!out) return 0;
    TuiEvent e;
    if (!tuiterm_poll(&e)) return 0;
    out->kind  = e.kind;
    out->key   = e.key;
    out->mouse = e.mouse;
    out->x = e.x; out->y = e.y;
    out->ctrl = e.ctrl; out->shift = e.shift; out->alt = e.alt;
    return 1;
}
