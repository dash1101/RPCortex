// The fake device. See fakehw.h for what it does and does not claim.
#include "rpc_app.h"
#include "fakehw.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

// --- state ------------------------------------------------------------------

struct PinScript {
    int      levels[FAKE_EDGES];
    unsigned hold_us[FAKE_EDGES];
    unsigned n;
    uint64_t t0;            // when the script started, on the fake clock (ns)
};

struct FakePin {
    int       driven;       // what a package last wrote
    unsigned  writes;
    int       dir;
    PinScript script;
    bool      scripted;
};

struct FakeI2cDev {
    bool          present;
    unsigned      addr;
    unsigned char resp[32];
    unsigned      n;
    unsigned      pos;
};

struct FakePio {
    bool           used;
    unsigned short prog[32];
    unsigned       prog_len;
    unsigned       divider;
    unsigned long  puts[256];
    unsigned       n_puts;
};

static FakePin     g_pin[FAKE_PINS];
static FakeI2cDev  g_i2c[FAKE_I2C_DEVS];
static int         g_adc[8];
static FakePio     g_pio[12];
static char        g_out[FAKE_OUT_MAX];
static unsigned    g_out_len;
static uint64_t    g_now_ns;
static uint32_t    g_clock_hz = 125000000u;
static bool     g_net_up;
static char     g_net_ssid[FW_NET_SSID_MAX];
static char     g_net_ip[FW_NET_ADDR_MAX];
static FwNetAp  g_aps[16];
static unsigned g_n_aps;
static char     g_url[128];
static char     g_body[2048];
static unsigned g_body_len;
static bool        g_i2c_up[2];
static bool        g_spi_up[2];

// --- what tests drive -------------------------------------------------------

void fake_reset(void) {
    memset(g_pin, 0, sizeof(g_pin));
    memset(g_i2c, 0, sizeof(g_i2c));
    memset(g_adc, 0, sizeof(g_adc));
    memset(g_pio, 0, sizeof(g_pio));
    memset(g_i2c_up, 0, sizeof(g_i2c_up));
    memset(g_spi_up, 0, sizeof(g_spi_up));
    g_net_up = false; g_n_aps = 0; g_url[0] = 0; g_body_len = 0;
    g_out_len = 0; g_out[0] = 0;
    g_now_ns = 1000000ull;
    g_clock_hz = 125000000u;
}

void fake_pin_script(unsigned pin, const int *levels, const unsigned *hold_us,
                     unsigned n) {
    if (pin >= FAKE_PINS || n > FAKE_EDGES) return;
    FakePin &p = g_pin[pin];
    memcpy(p.script.levels, levels, n * sizeof(int));
    memcpy(p.script.hold_us, hold_us, n * sizeof(unsigned));
    p.script.n  = n;
    p.script.t0 = g_now_ns;
    p.scripted  = true;
}

int      fake_pin_level(unsigned pin)  { return pin < FAKE_PINS ? g_pin[pin].driven : -1; }
unsigned fake_pin_writes(unsigned pin) { return pin < FAKE_PINS ? g_pin[pin].writes : 0; }

void fake_i2c_add(unsigned addr, const unsigned char *resp, unsigned n) {
    for (int i = 0; i < FAKE_I2C_DEVS; i++) {
        if (g_i2c[i].present) continue;
        g_i2c[i].present = true;
        g_i2c[i].addr    = addr;
        g_i2c[i].n       = n > sizeof(g_i2c[i].resp) ? sizeof(g_i2c[i].resp) : n;
        g_i2c[i].pos     = 0;
        if (resp && g_i2c[i].n) memcpy(g_i2c[i].resp, resp, g_i2c[i].n);
        return;
    }
}

void fake_adc_set(unsigned channel, int raw) {
    if (channel < 8) g_adc[channel] = raw;
}

const char *fake_output(void)   { return g_out; }
void fake_output_clear(void)    { g_out_len = 0; g_out[0] = 0; }
uint32_t fake_now_us(void)      { return (uint32_t)(g_now_ns / 1000ull); }
void fake_advance_us(uint32_t us) { g_now_ns += (uint64_t)us * 1000ull; }
void fake_set_clock_hz(uint32_t hz) { g_clock_hz = hz; }

void fake_net_up(const char *ssid, const char *ip) {
    g_net_up = true;
    snprintf(g_net_ssid, sizeof(g_net_ssid), "%s", ssid ? ssid : "test");
    snprintf(g_net_ip,   sizeof(g_net_ip),   "%s", ip   ? ip   : "192.168.1.50");
}
void fake_net_down(void) { g_net_up = false; }

void fake_net_add_ap(const char *ssid, int rssi, int channel, int secured) {
    if (g_n_aps >= 16) return;
    FwNetAp &a = g_aps[g_n_aps++];
    snprintf(a.ssid, sizeof(a.ssid), "%s", ssid ? ssid : "");
    a.rssi = rssi; a.channel = channel; a.secured = secured;
}

void fake_http_serve(const char *url, const char *body) {
    snprintf(g_url, sizeof(g_url), "%s", url ? url : "");
    g_body_len = 0;
    if (body) {
        unsigned n = (unsigned)strlen(body);
        if (n > sizeof(g_body)) n = sizeof(g_body);
        memcpy(g_body, body, n);
        g_body_len = n;
    }
}

unsigned fake_pio_program(int h, unsigned short *out, unsigned cap) {
    if (h < 0 || h >= 12 || !g_pio[h].used) return 0;
    unsigned n = g_pio[h].prog_len < cap ? g_pio[h].prog_len : cap;
    if (out) memcpy(out, g_pio[h].prog, n * sizeof(unsigned short));
    return g_pio[h].prog_len;
}
unsigned fake_pio_divider(int h) {
    return (h >= 0 && h < 12 && g_pio[h].used) ? g_pio[h].divider : 0;
}
unsigned fake_pio_puts(int h, unsigned long *out, unsigned cap) {
    if (h < 0 || h >= 12 || !g_pio[h].used) return 0;
    unsigned n = g_pio[h].n_puts < cap ? g_pio[h].n_puts : cap;
    if (out) memcpy(out, g_pio[h].puts, n * sizeof(unsigned long));
    return g_pio[h].n_puts;
}

// --- the ABI, as a package sees it ------------------------------------------

extern "C" {

int fw_printf(const char *fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    for (int i = 0; line[i] && g_out_len < FAKE_OUT_MAX - 1; i++)
        g_out[g_out_len++] = line[i];
    g_out[g_out_len] = 0;
    return n;
}

void  fw_log(int, const char *) {}
void *fw_malloc(size_t n) { return malloc(n); }
void  fw_free(void *p)    { free(p); }
uint32_t fw_millis(void)  { return (uint32_t)(g_now_ns / 1000000ull); }
uint32_t fw_micros(void)  { return (uint32_t)(g_now_ns / 1000ull); }
uint32_t fw_clock_hz(void) { return g_clock_hz; }

// Time passes when a package waits, which is what lets a scripted waveform be
// walked by a decoder that polls.
void fw_busy_wait_us(uint32_t us) { g_now_ns += (uint64_t)us * 1000ull; }
void fw_task_sleep_ms(uint32_t ms) { g_now_ns += (uint64_t)ms * 1000000ull; }
void fw_task_yield(void) { g_now_ns += 1000ull; }
int  fw_task_should_stop(void) { return 0; }
int  fw_task_self(void) { return 2; }
int  fw_task_kill(int) { return 0; }
uint32_t fw_cores(void) { return 2; }
uint32_t fw_core_id(void) { return 0; }
int  fw_task_spawn(const char *, int (*)(void *), void *, uint32_t) { return -1; }

uint32_t fw_heap_free(void)    { return 300000; }
uint32_t fw_heap_total(void)   { return 360000; }
uint32_t fw_heap_largest(void) { return 290000; }
void     fw_progress(const char *) {}

int rpc_register_command(const char *, const char *, int (*)(int, char **)) { return 0; }

// --- GPIO -------------------------------------------------------------------
//
// Pins 23-25 and 29 are reserved, matching the wireless boards — the case a
// package most needs to handle and the one hardest to remember to test.

unsigned fw_gpio_count(void) { return 30; }

int fw_gpio_usable(unsigned pin) {
    if (pin >= 30) return 0;
    if (pin == 23 || pin == 24 || pin == 25 || pin == 29) return 0;
    return 1;
}

int fw_gpio_init(unsigned pin, int dir) {
    if (!fw_gpio_usable(pin)) return -1;

    // Turning a scripted pin into an input restarts its waveform.
    //
    // That is the protocol, not a convenience: a one-wire part stays quiet
    // while the host is driving the line and begins its reply the moment the
    // host lets go. Timing the script from when the TEST set it up instead
    // would mean the response had already come and gone by the time the
    // package released the pin.
    if (dir == FW_PIN_IN && g_pin[pin].scripted && g_pin[pin].dir != FW_PIN_IN)
        g_pin[pin].script.t0 = g_now_ns;

    g_pin[pin].dir = dir;
    return 0;
}
int fw_gpio_pull(unsigned pin, int) { return fw_gpio_usable(pin) ? 0 : -1; }

int fw_gpio_put(unsigned pin, int value) {
    if (!fw_gpio_usable(pin)) return -1;
    g_pin[pin].driven = value ? 1 : 0;
    g_pin[pin].writes++;
    return 0;
}

// A scripted pin reports whatever its waveform says at the current instant.
// Anything else reads back what was last written, which is what a real pin
// wired to nothing does once it has been driven.
int fw_gpio_get(unsigned pin) {
    if (!fw_gpio_usable(pin)) return -1;

    // Reading a pin costs time, and modelling that is not a nicety: a decoder
    // that polls for an edge would otherwise spin for ever against a clock that
    // never moved.
    //
    // 250 ns, and the unit matters. At one microsecond per read the cost is a
    // twenty-sixth of the shortest pulse a DHT sends, and the error accumulates
    // over eighty polls into most of a bit — the decoder read four bytes
    // perfectly and got the fifth wrong, which looked exactly like a decoder
    // bug. The measurement has to be well finer than the thing measured.
    // 250 ns is about twelve cycles of load, compare and branch on this part.
    g_now_ns += 250;

    FakePin &p = g_pin[pin];
    if (!p.scripted) return p.driven;

    uint64_t elapsed_ns = g_now_ns - p.script.t0;
    uint64_t acc = 0;
    for (unsigned i = 0; i < p.script.n; i++) {
        acc += (uint64_t)p.script.hold_us[i] * 1000ull;
        if (elapsed_ns < acc) return p.script.levels[i];
    }
    return p.script.n ? p.script.levels[p.script.n - 1] : 1;   // idle high
}

// --- ADC --------------------------------------------------------------------

int      fw_adc_init(unsigned ch) { return ch <= 4 ? 0 : -1; }
int      fw_adc_read(unsigned ch) { return ch <= 4 ? g_adc[ch] : -1; }
unsigned fw_adc_temp_channel(void) { return 4; }

// --- I2C --------------------------------------------------------------------

static FakeI2cDev *i2c_find(unsigned addr) {
    for (int i = 0; i < FAKE_I2C_DEVS; i++)
        if (g_i2c[i].present && g_i2c[i].addr == addr) return &g_i2c[i];
    return nullptr;
}

int fw_i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned) {
    if (bus > 1) return -1;
    if (!fw_gpio_usable(sda) || !fw_gpio_usable(scl)) return -1;
    g_i2c_up[bus] = true;
    return 0;
}

// The SDK's semantics, deliberately: a transfer to an address nobody answers
// returns negative, and a ZERO-LENGTH transfer returns 0 without touching the
// bus. Getting that second part right here is the whole point — a fake that
// reported success for a zero-length write would have agreed with the bug that
// made i2cscan find 112 devices on an empty bus.
int fw_i2c_write(unsigned bus, unsigned addr, const void *, unsigned len, int) {
    if (bus > 1 || !g_i2c_up[bus] || addr > 0x7f) return -1;
    if (len == 0) return 0;
    return i2c_find(addr) ? (int)len : -1;
}

int fw_i2c_read(unsigned bus, unsigned addr, void *buf, unsigned len, int) {
    if (bus > 1 || !g_i2c_up[bus] || addr > 0x7f) return -1;
    if (len == 0) return 0;
    FakeI2cDev *d = i2c_find(addr);
    if (!d) return -1;
    unsigned char *out = (unsigned char *)buf;
    for (unsigned i = 0; i < len; i++)
        out[i] = d->n ? d->resp[d->pos++ % d->n] : 0xff;
    return (int)len;
}

int fw_i2c_deinit(unsigned bus) {
    if (bus > 1 || !g_i2c_up[bus]) return -1;
    g_i2c_up[bus] = false;
    return 0;
}

// --- SPI --------------------------------------------------------------------

int fw_spi_init(unsigned bus, unsigned sck, unsigned mosi, unsigned miso, unsigned baud) {
    if (bus > 1) return -1;
    if (!fw_gpio_usable(sck) || !fw_gpio_usable(mosi)) return -1;
    if (miso != 0xffffffffu && !fw_gpio_usable(miso)) return -1;
    g_spi_up[bus] = true;
    return (int)(baud ? baud : 1000000);
}
int fw_spi_set_baud(unsigned bus, unsigned baud) {
    return (bus <= 1 && g_spi_up[bus]) ? (int)baud : -1;
}
int fw_spi_write(unsigned bus, const void *, unsigned len) {
    return (bus <= 1 && g_spi_up[bus]) ? (int)len : -1;
}
int fw_spi_read(unsigned bus, void *buf, unsigned len, unsigned char fill) {
    if (bus > 1 || !g_spi_up[bus]) return -1;
    memset(buf, fill, len);
    return (int)len;
}
int fw_spi_transfer(unsigned bus, const void *tx, void *rx, unsigned len) {
    if (bus > 1 || !g_spi_up[bus]) return -1;
    memcpy(rx, tx, len);          // loopback, which is what a bare bus does
    return (int)len;
}
int fw_spi_deinit(unsigned bus) {
    if (bus > 1 || !g_spi_up[bus]) return -1;
    g_spi_up[bus] = false;
    return 0;
}

// --- PIO --------------------------------------------------------------------
//
// Recorded, not executed. What a test can meaningfully assert is that the right
// program was loaded and the right clock asked for — running the state machine
// would mean writing a PIO interpreter, which would be testing the interpreter.

unsigned fw_pio_count(void) { return 12; }
unsigned fw_pio_free(void) {
    unsigned n = 0;
    for (int i = 0; i < 12; i++) if (!g_pio[i].used) n++;
    return n;
}
int fw_pio_claim(void) {
    for (int i = 0; i < 12; i++)
        if (!g_pio[i].used) { memset(&g_pio[i], 0, sizeof(g_pio[i])); g_pio[i].used = true; return i; }
    return -1;
}
void fw_pio_release(int h) { if (h >= 0 && h < 12) g_pio[h].used = false; }

int fw_pio_load(int h, const unsigned short *prog, unsigned len,
                unsigned wrap_target, unsigned wrap) {
    if (h < 0 || h >= 12 || !g_pio[h].used || !prog || len == 0 || len > 32) return -1;
    if (wrap >= len || wrap_target >= len) return -1;
    memcpy(g_pio[h].prog, prog, len * sizeof(unsigned short));
    g_pio[h].prog_len = len;
    return 0;
}
int fw_pio_config_pins(int h, unsigned ob, unsigned oc, unsigned sb, unsigned sc,
                       unsigned ssb, unsigned ssc) {
    if (h < 0 || h >= 12 || !g_pio[h].used) return -1;
    if (oc  && !fw_gpio_usable(ob))  return -1;
    if (sc  && !fw_gpio_usable(sb))  return -1;
    if (ssc && !fw_gpio_usable(ssb)) return -1;
    return 0;
}
int fw_pio_config_shift(int h, int, int, unsigned t) {
    if (h < 0 || h >= 12 || !g_pio[h].used) return -1;
    return t > 32 ? -1 : 0;
}
int fw_pio_config_clock(int h, unsigned div_x256) {
    if (h < 0 || h >= 12 || !g_pio[h].used || div_x256 == 0) return -1;
    g_pio[h].divider = div_x256;
    return 0;
}
int  fw_pio_start(int h) { return (h >= 0 && h < 12 && g_pio[h].prog_len) ? 0 : -1; }
void fw_pio_stop(int) {}
int  fw_pio_put(int h, unsigned long v, unsigned) {
    if (h < 0 || h >= 12 || !g_pio[h].used) return -1;
    if (g_pio[h].n_puts < 256) g_pio[h].puts[g_pio[h].n_puts++] = v;
    return 0;
}
int fw_pio_get(int h, unsigned long *out, unsigned) {
    if (h < 0 || h >= 12 || !g_pio[h].used || !out) return -1;
    return 0;
}

// --- network ----------------------------------------------------------------
//
// A fake network, so a package that fetches or scans is testable the same way
// one that reads a pin is. What it models is the SHAPE: connected or not, a
// list of access points, a name that resolves, and a URL that returns bytes.

int fw_net_connected(void) { return g_net_up ? 1 : 0; }

int fw_net_ssid(char *out, unsigned cap) {
    if (!out || !cap) return 0;
    int n = snprintf(out, cap, "%s", g_net_up ? g_net_ssid : "");
    return n < 0 ? 0 : n;
}
int fw_net_ip(char *out, unsigned cap) {
    if (!out || !cap) return 0;
    int n = snprintf(out, cap, "%s", g_net_up ? g_net_ip : "");
    return n < 0 ? 0 : n;
}

int fw_net_scan(FwNetAp *out, unsigned max) {
    if (!out || !max) return -1;
    unsigned n = g_n_aps < max ? g_n_aps : max;
    for (unsigned i = 0; i < n; i++) out[i] = g_aps[i];
    return (int)n;
}

int fw_net_resolve(const char *host, char *out, unsigned cap) {
    if (!host || !out || !cap) return -1;
    if (!g_net_up) return -1;
    // Anything resolves while connected, to a fixed address. A test that cares
    // which address it got is testing the fake.
    int n = snprintf(out, cap, "93.184.216.34");
    return n < 0 ? -1 : n;
}

int fw_http_get(const char *url, void *buf, unsigned cap) {
    if (!url || !buf || !cap || !g_net_up) return -1;
    if (g_url[0] && strcmp(url, g_url) != 0) return -1;   // only the one set up
    unsigned n = g_body_len < cap ? g_body_len : cap;
    memcpy(buf, g_body, n);
    return (int)n;
}

int fw_http_download(const char *url, const char *) {
    if (!url || !g_net_up) return -1;
    return (int)g_body_len;
}

// --- files ------------------------------------------------------------------

int      fw_file_write(const char *, const void *, uint32_t) { return 0; }
uint32_t fw_file_read(const char *, void *, uint32_t) { return 0; }
int      fw_file_remove(const char *) { return 0; }
int      fw_file_exists(const char *) { return 0; }

}  // extern "C"
