// Desc: One source of truth for pins and buses. Board profiles, and where a pin came from.
// File: novaboard.cpp
#include "novaboard.h"
#include "novacore.h"

#include <string.h>
#include <stdio.h>

namespace nova {
namespace board {

// --- the signals ------------------------------------------------------------

static const char *const kNames[PIN_COUNT] = {
    "sda", "scl",
    "enc_a", "enc_b", "enc_sw", "btn1", "btn2", "killsw",
    "spi_sck", "spi_mosi", "spi_miso",
    "cc_cs", "cc_gdo0",
    "sx_cs", "sx_rst",
    "sd_cs",
    "ir_tx", "ir_rx",
    "gps_tx", "gps_rx",
    "buzzer", "vibe", "led", "dht", "ibutton",
    "battery", "vbus",
};

const char *name(PinId id) {
    return (id >= 0 && id < PIN_COUNT) ? kNames[id] : "?";
}

PinId by_name(const char *s) {
    if (!s || !*s) return PIN_COUNT;
    // The full registry key is accepted as well as the short name. Both
    // conventions exist in the drivers and in people's notes, and rejecting one
    // of them would only mean a lookup that fails for no reason a user can see.
    static const char kPrefix[] = NOVA_KEY_PREFIX "PIN_";
    if (strncmp(s, kPrefix, sizeof(kPrefix) - 1) == 0) s += sizeof(kPrefix) - 1;
    for (int i = 0; i < PIN_COUNT; i++)
        if (nova::ieq(s, kNames[i])) return (PinId)i;
    return PIN_COUNT;
}

static void key_for(PinId id, char *out, unsigned cap) {
    snprintf(out, cap, NOVA_KEY_PREFIX "PIN_%s", name(id));
}

// --- the profiles -----------------------------------------------------------
//
// A profile is a flat array indexed by PinId. PIN_NONE means the profile has
// nothing to say and the caller's fallback decides.

struct Profile {
    const char *id;
    const char *display;      // what a person calls this board
    const char *disp_bus;     // "i2c" or "spi"
    const char *notes;
    int8_t      pins[PIN_COUNT];
};

// Raspberry Pi Pico 2 W — the reference board.
//
// SD shares SPI0 with the radios rather than taking a second bus: it trades a
// rare LoRa-receive / SD-write collision for the three pins a split would cost,
// which the kill-switch button and the remaining headroom now use.
//
// No 'led' default ON PURPOSE. On Pico W-class boards the onboard LED hangs off
// the CYW43 module and is not addressable as a GPIO at all, so a default here
// meant every alert blinked a pin with nothing wired to it while the real LED
// stayed dark. Wiring an external one is still `d1 pins set led <gpio>`.
//
// 'battery' and 'vbus' are OPT-IN and must never get a profile default. The
// power module reads them only when they are configured, and a default would
// switch off the guard that stops an unwired floating ADC reporting a confident,
// wrong battery level.
//
// 'ibutton' stays unassigned: the map has three pins left and the LF front-end
// or an iButton reader needs a GPIO expander to collapse the buttons first.
static const Profile kPico2W = {
    "pico2w", "Raspberry Pi Pico 2 W", "i2c",
    "Display on I2C. SD shares the radio SPI0 bus. Kill-switch on GPIO 8. "
    "iButton and an LF front-end need a GPIO expander.",
    {
        /* sda      */  4, /* scl      */  5,
        /* enc_a    */ 14, /* enc_b    */ 15, /* enc_sw */ 13,
        /* btn1     */ 22, /* btn2     */ 26, /* killsw */  8,
        /* spi_sck  */ 18, /* spi_mosi */ 19, /* spi_miso */ 16,
        /* cc_cs    */ 17, /* cc_gdo0  */ 20,
        /* sx_cs    */ 21, /* sx_rst   */ 12,
        /* sd_cs    */  9,
        /* ir_tx    */  6, /* ir_rx    */  7,
        /* gps_tx   */  0, /* gps_rx   */  1,
        /* buzzer   */ 27, /* vibe     */ 28, /* led */ PIN_NONE,
        /* dht      */  3, /* ibutton  */ PIN_NONE,
        /* battery  */ PIN_NONE, /* vbus */ PIN_NONE,
    }
};

// Pimoroni Pico Plus 2 W — a drop-in upgrade with the same header, so it takes
// the same map. Sharing the array rather than copying it is what stops the two
// drifting; they cannot disagree because there is only one of them.
static const Profile kPicoPlus2W = {
    "picoplus2w", "Pimoroni Pico Plus 2 W", "i2c",
    "Same header as the Pico 2 W, so the same map. Adds 8 MB PSRAM, 16 MB flash "
    "and GPIO beyond 28 — put iButton and the LF front-end there.",
    {
        4, 5, 14, 15, 13, 22, 26, 8, 18, 19, 16, 17, 20, 21, 12, 9,
        6, 7, 0, 1, 27, 28, PIN_NONE, 3, PIN_NONE, PIN_NONE, PIN_NONE,
    }
};

static const Profile *const kProfiles[] = { &kPico2W, &kPicoPlus2W };
#define PROFILE_COUNT (sizeof(kProfiles) / sizeof(kProfiles[0]))

unsigned profile_count(void) { return PROFILE_COUNT; }
const char *profile_id(unsigned i) { return i < PROFILE_COUNT ? kProfiles[i]->id : ""; }

// Which pins the profile refuses to default, whatever a profile array says. The
// array above already leaves them at PIN_NONE; this is the belt to that pair of
// braces, so a future profile written in a hurry cannot quietly undo the guard.
static bool opt_in(PinId id) { return id == PIN_BATTERY || id == PIN_VBUS; }

// --- which board -------------------------------------------------------------

const char *detect(void) {
    char b[24];
    if (!fw_board(b, sizeof(b))) return nullptr;
    if (nova::ieq(b, "pico2_w"))    return "pico2w";
    if (nova::ieq(b, "pico_plus2_w")) return "picoplus2w";
    if (nova::ieq(b, "pimoroni_pico_plus2w")) return "picoplus2w";
    // A non-wireless Pico 2 runs everything except the radios and shares the
    // header, so the map still applies — the WiFi and BLE screens simply report
    // no radio, which is true.
    if (nova::ieq(b, "pico2"))      return "pico2";
    // An RP2040 board resolves to nothing deliberately. See the header.
    return nullptr;
}

static const Profile *find(const char *id) {
    if (!id || !*id) return nullptr;
    // The old spelling still resolves. Somebody's device has it in the registry
    // and an update that made their pins vanish would be a bad trade for tidiness.
    if (nova::ieq(id, "rp2350")) id = "pico2w";
    if (nova::ieq(id, "pico2"))  id = "pico2w";
    for (unsigned i = 0; i < PROFILE_COUNT; i++)
        if (nova::ieq(id, kProfiles[i]->id)) return kProfiles[i];
    return nullptr;
}

static const Profile *active(void) {
    const char *cfg = nova::reg(NOVA_KEY_PREFIX "Board", "");
    if (cfg[0] && !nova::ieq(cfg, "auto")) {
        const Profile *p = find(cfg);
        if (p) return p;
    }
    return find(detect());
}

const char *board_id(void)   { const Profile *p = active(); return p ? p->id : "unknown"; }
const char *board_name(void) { const Profile *p = active(); return p ? p->display : "unrecognised board"; }
const char *display_bus(void) { const Profile *p = active(); return p ? p->disp_bus : "i2c"; }

bool board_set(const char *id) {
    if (!id || !*id) return false;
    if (nova::ieq(id, "auto")) {
        nova::reg_set(NOVA_KEY_PREFIX "Board", "");
        return true;
    }
    if (!find(id)) return false;
    nova::reg_set(NOVA_KEY_PREFIX "Board", id);
    return true;
}

// --- resolving a pin ---------------------------------------------------------

Source source(PinId id) {
    if (id < 0 || id >= PIN_COUNT) return SRC_NONE;
    char key[48];
    key_for(id, key, sizeof(key));
    if (fw_reg_has(key)) return SRC_REG;
    const Profile *p = active();
    if (p && !opt_in(id) && p->pins[id] != PIN_NONE) return SRC_PROFILE;
    return SRC_NONE;
}

int pin(PinId id, int fallback) {
    if (id < 0 || id >= PIN_COUNT) return fallback;

    char key[48];
    key_for(id, key, sizeof(key));
    // The override wins over everything, including a profile that disagrees and
    // including the opt-in guard: someone who typed a pin number meant it.
    int v = nova::reg_int(key, -2);
    if (v != -2) return v;

    const Profile *p = active();
    if (p && !opt_in(id) && p->pins[id] != PIN_NONE) return p->pins[id];
    return fallback;
}

int pin(const char *nm, int fallback) {
    PinId id = by_name(nm);
    return id == PIN_COUNT ? fallback : pin(id, fallback);
}

void set(PinId id, int gpio) {
    if (id < 0 || id >= PIN_COUNT) return;
    char key[48];
    key_for(id, key, sizeof(key));
    nova::reg_set_int(key, gpio);
}

void clear(PinId id) {
    if (id < 0 || id >= PIN_COUNT) return;
    char key[48];
    key_for(id, key, sizeof(key));
    nova::reg_set(key, "");
}

// --- validation --------------------------------------------------------------

bool reserved(int gpio) {
    // Ask the firmware rather than hard-coding 23/24/25/29: it knows whether
    // this board has a radio at all, and a non-wireless Pico 2 has those pins
    // free.
    return gpio >= 0 && !fw_gpio_usable((unsigned)gpio);
}

// The three peripheral groupings, each taken from the RP2040/RP2350 function
// table rather than from a pattern that happens to fit the first few pins.
//
// SPI repeats every 8: role is gpio % 4 (0 = RX/MISO, 1 = CSn, 2 = SCK,
// 3 = TX/MOSI) and the controller is (gpio / 8) % 2. Checked against the real
// table: 0-7 SPI0, 8-15 SPI1, 16-23 SPI0, 24-31 SPI1.
static int spi_ctrl(int g) { return (g / 8) % 2; }
static int spi_role(int g) { return g % 4; }

// I2C alternates every two pins: 0/1 = I2C0 SDA/SCL, 2/3 = I2C1 SDA/SCL, 4/5 =
// I2C0 again.
static int i2c_ctrl(int g) { return (g / 2) % 2; }
static int i2c_role(int g) { return g % 2; }     // 0 = SDA, 1 = SCL

// UART does NOT follow the SPI grouping, which is the easy mistake here. By
// four-pin group the instance runs 0,1,1,0,0,1,1,0 — GP0 is UART0, GP4 and GP8
// are UART1, GP12 and GP16 are UART0, GP20 and GP24 are UART1. That is
// ((group + 1) / 2) & 1, and using (gpio / 8) % 2 instead puts GP8 on the wrong
// controller.
static int uart_ctrl(int g) { return (((g / 4) + 1) >> 1) & 1; }
static int uart_role(int g) { return g % 4; }    // 0 = TX, 1 = RX, 2 = CTS, 3 = RTS

namespace {
// Lines are formatted by the caller and appended finished.
//
// A printf-style wrapper would be tidier and is not available: the ABI exports
// snprintf but not vsnprintf, so there is no way to forward a va_list. Each site
// formats into its own small buffer instead, which is honest about what it costs
// and cannot get its arguments out of step with its format string.
struct Report {
    char    *out;
    unsigned cap;
    unsigned at;
    unsigned count;
};
void say(Report &r, const char *line) {
    r.count++;
    if (!r.cap || r.at + 2 >= r.cap) return;
    r.at += nova::copy(r.out + r.at, r.cap - r.at, line);
    if (r.at + 1 < r.cap) { r.out[r.at++] = '\n'; r.out[r.at] = 0; }
}
}  // namespace

unsigned check(char *out, unsigned cap) {
    Report r = { out, cap, 0, 0 };
    char line[96];
    if (cap) out[0] = 0;

    if (!active()) {
        say(r, "No board profile. `d1 pins board <id>` to choose one.");
        return r.count;
    }

    // Every assigned pin, once: in range, not the board's, not doubled up.
    int assigned[PIN_COUNT];
    int nassigned = 0;
    for (int i = 0; i < PIN_COUNT; i++) {
        int g = pin((PinId)i);
        if (g == PIN_NONE) continue;

        if (g < 0 || (unsigned)g >= fw_gpio_count()) {
            snprintf(line, sizeof(line), "%s is GPIO %d, which this board does not have.",
                     name((PinId)i), g);
            say(r, line);
            continue;
        }
        if (reserved(g)) {
            snprintf(line, sizeof(line), "%s is GPIO %d, which the board keeps for its radio.",
                     name((PinId)i), g);
            say(r, line);
            continue;
        }
        // Naming BOTH signals, because "GPIO 20 is used twice" leaves the reader
        // to find the other one and the answer is right here.
        for (int j = 0; j < nassigned; j++) {
            if (pin((PinId)assigned[j]) != g) continue;
            snprintf(line, sizeof(line), "GPIO %d is both %s and %s.",
                     g, name((PinId)assigned[j]), name((PinId)i));
            say(r, line);
            break;
        }
        assigned[nassigned++] = i;
    }

    // I2C: SDA and SCL must be the two roles of the SAME controller.
    int sda = pin(PIN_SDA), scl = pin(PIN_SCL);
    if (sda != PIN_NONE && scl != PIN_NONE) {
        if (i2c_ctrl(sda) != i2c_ctrl(scl)) {
            snprintf(line, sizeof(line), "I2C: GPIO %d is on I2C%d and GPIO %d on I2C%d.",
                     sda, i2c_ctrl(sda), scl, i2c_ctrl(scl));
            say(r, line);
        } else if (i2c_role(sda) != 0 || i2c_role(scl) != 1) {
            snprintf(line, sizeof(line), "I2C: GPIO %d cannot be SDA and %d SCL on RP2.", sda, scl);
            say(r, line);
        }
    }

    // SPI: all three on one controller, each in its own role.
    int sck = pin(PIN_SPI_SCK), mosi = pin(PIN_SPI_MOSI), miso = pin(PIN_SPI_MISO);
    if (sck != PIN_NONE && mosi != PIN_NONE && miso != PIN_NONE) {
        if (spi_ctrl(sck) != spi_ctrl(mosi) || spi_ctrl(sck) != spi_ctrl(miso)) {
            snprintf(line, sizeof(line), "SPI: GPIO %d, %d and %d are not all on one controller.",
                     sck, mosi, miso);
            say(r, line);
        }
        if (spi_role(sck) != 2) {
            snprintf(line, sizeof(line), "SPI: GPIO %d cannot be SCK on RP2.", sck);
            say(r, line);
        }
        if (spi_role(mosi) != 3) {
            snprintf(line, sizeof(line), "SPI: GPIO %d cannot be MOSI on RP2.", mosi);
            say(r, line);
        }
        if (spi_role(miso) != 0) {
            snprintf(line, sizeof(line), "SPI: GPIO %d cannot be MISO on RP2.", miso);
            say(r, line);
        }
    }

    // The GPS UART. These are named from the RP2's point of view — gps_tx is the
    // pin the RP2 transmits ON, which goes to the module's RX — so the roles
    // wanted are TX and RX in that order.
    int gtx = pin(PIN_GPS_TX), grx = pin(PIN_GPS_RX);
    if (gtx != PIN_NONE && grx != PIN_NONE) {
        if (uart_ctrl(gtx) != uart_ctrl(grx)) {
            snprintf(line, sizeof(line), "GPS: GPIO %d is on UART%d and GPIO %d on UART%d.",
                     gtx, uart_ctrl(gtx), grx, uart_ctrl(grx));
            say(r, line);
        } else if (uart_role(gtx) != 0 || uart_role(grx) != 1) {
            snprintf(line, sizeof(line), "GPS: GPIO %d cannot be UART TX and %d RX on RP2.",
                     gtx, grx);
            say(r, line);
        }
    }

    return r.count;
}

}  // namespace board
}  // namespace nova
