// Desc: Battery level and USB power, and the honest gaps in both.
// File: novapower.cpp
#include "novapower.h"
#include "novaboard.h"
#include "novacore.h"

#include "rpc_app.h"

namespace nova {
namespace power {

// A lithium cell is 3.0 V flat and 4.2 V full. The window either side is what
// separates a reading from a floating pin: below 2.5 V nothing is connected,
// above 4.6 V it is not a single cell.
#define MV_EMPTY   3000
#define MV_FULL    4200
#define MV_FLOOR   2500
#define MV_CEILING 4600

// Cached, because the ADC read crosses the ABI and the status bar asks on every
// frame. Five seconds is far faster than a battery moves.
#define CACHE_MS 5000

// The source gets its own, shorter, cache. A cell moves over hours, but plugging
// a cable in is something somebody does and then looks at the screen to confirm
// — five seconds of the old icon would read as a dead socket.
#define SRC_CACHE_MS 1000

static uint32_t g_at;
static int      g_mv = -1;
static bool     g_have;

static uint32_t g_src_at;
static Source   g_src;
static bool     g_src_have;

static int read_mv(void) {
    int pin = board::pin(board::PIN_BATTERY);
    if (pin == board::PIN_NONE) return 0;

    // The ADC channels are GPIO 26-29, so the channel is the pin minus 26. A
    // battery pin outside that range is somebody's typo, and reading channel
    // -3 would return something rather than nothing.
    int ch = pin - 26;
    if (ch < 0 || ch > 3) return 0;

    fw_adc_init((unsigned)ch);
    int raw = fw_adc_read((unsigned)ch);
    if (raw < 0) return 0;

    // 12 bits over 3.3 V, then back through the divider. Two 100k resistors
    // halve it, so the default multiplier is 2 — configurable because somebody
    // will use a different pair.
    int div = nova::reg_int(NOVA_KEY_PREFIX "BattDiv", 2);
    if (div < 1) div = 1;
    return (raw * 3300 / 4095) * div;
}

int millivolts(void) {
    uint32_t now = fw_millis();
    if (g_have && now - g_at < CACHE_MS) return g_mv;
    g_at = now;
    g_have = true;
    int mv = read_mv();
    // Outside the window is an unwired pin, not a reading. Reporting zero says
    // "nothing to see" where reporting 1800 mV would say "your battery is
    // critically flat" about a battery that is not there.
    g_mv = (mv >= MV_FLOOR && mv <= MV_CEILING) ? mv : 0;
    return g_mv;
}

int percent(void) {
    int mv = millivolts();
    if (!mv) return -1;
    if (mv >= MV_FULL) return 100;
    if (mv <= MV_EMPTY) return 0;
    return (mv - MV_EMPTY) * 100 / (MV_FULL - MV_EMPTY);
}

static Source read_source(void) {
    int vbus = board::pin(board::PIN_VBUS);
    if (vbus != board::PIN_NONE) {
        // An explicitly configured pin wins. Somebody who has wired a divider to
        // a spare GPIO has said something specific about this device, and it is
        // also the only answer available with the radio down.
        fw_gpio_init((unsigned)vbus, FW_PIN_IN);
        // No pull. VBUS sense is a divider off the USB rail and driving either
        // way against it gives a reading about the pull-up rather than the rail.
        fw_gpio_pull((unsigned)vbus, FW_PULL_NONE);
        if (fw_gpio_get((unsigned)vbus) == 1) return PWR_USB;
        return PWR_BATTERY;
    }

    // Otherwise the firmware answers, because a package cannot. VBUS sense on a
    // Pico W and a Pico 2 W is GPIO 2 on the CYW43 module, reachable only
    // through the radio driver, and the pin it lives on elsewhere is one of the
    // four the firmware keeps for the radio and refuses to hand out. Every
    // attempt to read it from here got either a refusal or a floating value.
    //
    // UNKNOWN comes back whenever the question could not be asked — with the
    // radio down, which is most of the time — and it is a real answer rather
    // than a failure. It must not be drawn as an empty battery.
    switch (fw_power_source()) {
        case FW_POWER_USB:     return PWR_USB;
        case FW_POWER_BATTERY: return PWR_BATTERY;
        default:               break;
    }

    // Nothing knows. A wired battery divider at least says the device is running
    // off something with a level, which is more than nothing.
    return percent() >= 0 ? PWR_BATTERY : PWR_UNKNOWN;
}

Source source(void) {
    uint32_t now = fw_millis();
    if (g_src_have && now - g_src_at < SRC_CACHE_MS) return g_src;
    g_src_at = now;
    g_src_have = true;
    g_src = read_source();
    return g_src;
}

bool low(void) {
    int p = percent();
    if (p < 0) return false;
    return p <= nova::reg_int(NOVA_KEY_PREFIX "LowPct", 15);
}

}  // namespace power
}  // namespace nova
