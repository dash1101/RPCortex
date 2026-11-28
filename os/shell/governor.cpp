// The clock governor: lowers the system clock while the device is quiet and on
// battery, and puts it back the moment there is work or anything clock-timed
// is in use. The decision is core/governor.cpp (host-tested); this is the part
// that reads the board and changes the clock.
//
// What it will not do, and why:
//   - change the clock while an SPI, I2C or UART block is enabled, a PWM slice
//     is running, or a PIO state machine is claimed (other than the radio's,
//     which radio.cpp re-times itself). It reads the hardware for this rather
//     than keeping a count, so a package that exits without closing its bus, or
//     a firmware driver that leaves its bus up (the SD card does), holds the
//     clock exactly as long as it should.
//   - race a set-up. Every set-up takes ClockUse (clockuse.h): the same lock
//     this takes to look and change, and the clock back to its base first.
//   - go above the base: the boot clock, or what `pulse set` last asked for.
//
// Power saved by a lower clock is DEVICE-UNCONFIRMED (no meter on the bench),
// and so is the battery reading itself: both bench boards are always on USB.
#include "clockuse.h"
#include "governor.h"
#include "lock.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "radio.h"

#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"

bool clock_set_mhz(unsigned mhz);           // sys.cpp: the change itself
int  net_power_source(void);                // net.cpp / net_off.cpp

static RpcLock   g_clock_lock;
static unsigned  g_base_mhz;                // 0 until the governor starts
static bool      g_lowered;                 // the governor has it below the base
static GovMode   g_mode = GOV_AUTO;
static GovState  g_st;
static GovReason g_why = GOVR_OFF;
static const char *g_holder;
static unsigned  g_changes;

static unsigned cur_mhz(void) { return clock_get_hz(clk_sys) / 1000000u; }

// --- what is using the clock -----------------------------------------------

static const char *clocked_holder(void) {
    if (spi_get_hw(spi0)->cr1 & SPI_SSPCR1_SSE_BITS) return "spi0";
    if (spi_get_hw(spi1)->cr1 & SPI_SSPCR1_SSE_BITS) return "spi1";
    if (i2c_get_hw(i2c0)->enable & I2C_IC_ENABLE_ENABLE_BITS) return "i2c0";
    if (i2c_get_hw(i2c1)->enable & I2C_IC_ENABLE_ENABLE_BITS) return "i2c1";
    if (uart_get_hw(uart0)->cr & UART_UARTCR_UARTEN_BITS) return "uart0";
    if (uart_get_hw(uart1)->cr & UART_UARTCR_UARTEN_BITS) return "uart1";
    if (pwm_hw->en) return "pwm";
    unsigned rp = 0, rs = 0;
    bool radio = radio_bus_sm(&rp, &rs);
    for (unsigned p = 0; p < NUM_PIOS; p++)
        for (unsigned sm = 0; sm < NUM_PIO_STATE_MACHINES; sm++) {
            if (radio && p == rp && sm == rs) continue;
            if (pio_sm_is_claimed(PIO_INSTANCE(p), sm)) return "pio";
        }
    return nullptr;
}

// --- ClockUse ----------------------------------------------------------------

void clock_use_begin(void) {
    lock_acquire(&g_clock_lock);
    if (g_lowered && g_base_mhz) {
        if (clock_set_mhz(g_base_mhz)) { g_lowered = false; g_changes++; }
    }
}

void clock_use_end(void) { lock_release(&g_clock_lock); }

uint32_t clock_base_hz(void) {
    return g_base_mhz ? g_base_mhz * 1000000u : clock_get_hz(clk_sys);
}

bool clock_set_base_mhz(unsigned mhz) {
    LockGuard _g(&g_clock_lock);
    if (!clock_set_mhz(mhz)) return false;
    g_base_mhz = cur_mhz();
    g_lowered = false;
    g_st.quiet = false;
    return true;
}

// --- the power source ----------------------------------------------------------

int power_source(void) {
#ifdef PICO_VBUS_PIN
    // The Pico and Pico 2 sense VBUS on a pin of their own. Read with the pin as
    // a plain input, which is all gpio_init leaves it as.
    static bool ready;
    if (!ready) { gpio_init(PICO_VBUS_PIN); gpio_set_dir(PICO_VBUS_PIN, false); ready = true; }
    return gpio_get(PICO_VBUS_PIN) ? 1 : 0;
#else
    // The W boards sense it through the radio chip: the cached reading, never a
    // conversation with the chip of this task's own (net.cpp says why). -1 while
    // the radio is off, which the governor treats as plugged in.
    return net_power_source();
#endif
}

// --- the governor ----------------------------------------------------------------

static unsigned floor_mhz(void) {
    unsigned f = (unsigned)atoi(reg_get("Power.Clock_Floor", "0"));
    return f >= GOV_FLOOR_MHZ ? f : GOV_FLOOR_MHZ;
}

// Load over the last step as a share of ONE core: a single task running flat
// out is 100%, whichever core it is on and however idle the other. Averaging
// over both cores (task_cpu_percent) read that as 50%, and a board pinned at
// the floor ran pretest's SHA-256 at a third of its speed without scaling up.
static unsigned gov_load(void) {
    static uint32_t last_busy, last_wall;
    uint32_t busy = task_busy_ms(), wall = task_now_ms();
    uint32_t db = busy - last_busy, dw = wall - last_wall;
    bool fell = (int32_t)db < 0;              // a busy task exited
    last_busy = busy; last_wall = wall;
    if (fell || !dw) return 0;
    uint32_t pct = (uint32_t)((uint64_t)db * 100u / dw);
    return pct > 100 ? 100 : pct;
}

static void gov_step(void) {
    GovIn in;
    in.mode = g_mode;
    in.power = power_source();
    in.load = gov_load();
    in.now_ms = task_now_ms();
    in.floor_mhz = floor_mhz();

    LockGuard _g(&g_clock_lock);
    g_holder = clocked_holder();
    in.held = g_holder != nullptr;
    in.base_mhz = g_base_mhz;
    in.cur_mhz = cur_mhz();
    unsigned want = gov_decide(&in, &g_st, &g_why);
    if (want == in.cur_mhz) return;
    if (!clock_set_mhz(want)) return;
    g_lowered = cur_mhz() < g_base_mhz;
    g_changes++;
    log_addf(LOG_K_INFO, "[governor] %u -> %u MHz (%s)", in.cur_mhz, cur_mhz(), gov_reason_str(g_why));
}

// A quarter second: a burst of work gets the base clock within 250 ms. At a
// second, a short job (pretest's checks, a TLS handshake) was over before the
// governor noticed it and ran entirely at the floor. Going DOWN still takes
// GOV_SETTLE_MS of quiet, so the faster look costs nothing in flapping.
#define GOV_STEP_MS 250

static int gov_task(void *) {
    for (;;) {
        task_sleep_ms(GOV_STEP_MS);
        gov_step();
    }
}

void governor_start(void) {
    if (g_base_mhz) return;
    g_base_mhz = cur_mhz();                 // after the boot clock, if any
    gov_parse_mode(reg_get("Power.Governor", "auto"), &g_mode);
    task_spawn("governor", "(kernel)", gov_task, nullptr, TASK_STACK_MIN, AFFINITY_CORE0);
}

bool governor_set_mode(const char *mode) {
    GovMode m;
    if (!gov_parse_mode(mode, &m)) return false;
    reg_set("Power.Governor", gov_mode_str(m));
    LockGuard _g(&g_clock_lock);
    g_mode = m;
    g_st.quiet = false;
    // Off means off now, not at the next step.
    if (m == GOV_OFF && g_lowered && clock_set_mhz(g_base_mhz)) { g_lowered = false; g_changes++; }
    return true;
}

void governor_status(GovStatus *o) {
    LockGuard _g(&g_clock_lock);
    o->mode = gov_mode_str(g_mode);
    o->why = gov_reason_str(g_why);
    o->holder = clocked_holder();
    o->base_mhz = g_base_mhz ? g_base_mhz : cur_mhz();
    o->floor_mhz = floor_mhz();
    o->cur_mhz = cur_mhz();
    o->changes = g_changes;
}
