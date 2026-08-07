// Desc: The hardware registry — every module, what it needs, and what it becomes on the home screen.
// File: novamodtab.cpp
#include "novamodtab.h"
#include "novacore.h"

#include "rpc_app.h"
#include <string.h>

namespace nova {

using board::PinId;

const char *category_name(Category c) {
    switch (c) {
        case CAT_WIRELESS: return "Wireless";
        case CAT_SENSORS:  return "Sensors";
        case CAT_TOOLS:    return "Tools";
        case CAT_SYSTEM:   return "System";
        default:           return "Testing";
    }
}

const char *bus_name(BusKind b) {
    switch (b) {
        case BUS_I2C:     return "I2C";
        case BUS_SPI:     return "SPI";
        case BUS_UART:    return "UART";
        case BUS_GPIO:    return "GPIO";
        case BUS_PWM:     return "PWM";
        case BUS_ADC:     return "ADC";
        case BUS_ONEWIRE: return "1-wire";
        default:          return "onboard";
    }
}

// --- what each module is wired to --------------------------------------------
//
// SIGNAL NAMES, not GPIO numbers. On the reference Pico 2 W profile these come
// out as: I2C on 4/5, SPI0 on 18/19/16 shared by the sub-GHz radio, the LoRa
// radio and the SD card with separate selects on 17, 21 and 9; IR on 6/7; the
// GPS UART on 0/1; the buzzer on 27, the vibration motor on 28, the DHT on 3.
// None of that is written here, and changing it means editing novaboard alone.

static const PinId kPinsDht[]     = { board::PIN_DHT };
static const PinId kPinsGps[]     = { board::PIN_GPS_TX, board::PIN_GPS_RX };
static const PinId kPinsI2C[]     = { board::PIN_SDA, board::PIN_SCL };
static const PinId kPinsCc[]      = { board::PIN_CC_CS, board::PIN_CC_GDO0,
                                      board::PIN_SPI_SCK, board::PIN_SPI_MOSI,
                                      board::PIN_SPI_MISO };
static const PinId kPinsSx[]      = { board::PIN_SX_CS, board::PIN_SX_RST,
                                      board::PIN_SPI_SCK, board::PIN_SPI_MOSI,
                                      board::PIN_SPI_MISO };
static const PinId kPinsSd[]      = { board::PIN_SD_CS,
                                      board::PIN_SPI_SCK, board::PIN_SPI_MOSI,
                                      board::PIN_SPI_MISO };
static const PinId kPinsIrRx[]    = { board::PIN_IR_RX };
static const PinId kPinsIrTx[]    = { board::PIN_IR_TX };
static const PinId kPinsIbutton[] = { board::PIN_IBUTTON };
static const PinId kPinsBattery[] = { board::PIN_BATTERY };
static const PinId kPinsBuzzer[]  = { board::PIN_BUZZER };
static const PinId kPinsVibe[]    = { board::PIN_VIBE };
static const PinId kPinsLed[]     = { board::PIN_LED };

#define M(arr) arr, (uint8_t)(sizeof(arr) / sizeof(arr[0]))

// The order here is the order `d1 scan` reports and the order the Hardware
// screen lists. Radios first, because they are what somebody is usually here
// for; the small stuff last.
static const Module kModules[] = {
    // id           label        chip          bus          category      pins             diag  addr
    { "bt",        "BLE",       "CYW43439",   BUS_NONE,    CAT_WIRELESS, nullptr, 0,       false, 0x00 },
    { "pn532",     "NFC",       "PN532",      BUS_I2C,     CAT_WIRELESS, M(kPinsI2C),      false, 0x24 },
    { "cc1101",    "Sub-GHz",   "CC1101",     BUS_SPI,     CAT_WIRELESS, M(kPinsCc),       false, 0x00 },
    { "sx1276",    "LoRa",      "SX1276",     BUS_SPI,     CAT_WIRELESS, M(kPinsSx),       false, 0x00 },
    { "ir_rx",     "IR",        "VS1838B",    BUS_GPIO,    CAT_WIRELESS, M(kPinsIrRx),     false, 0x00 },
    // The transmitter is a separate part on a separate pin, and it can be fitted
    // without the receiver — a universal remote needs only the LED. It shares the
    // IR app rather than getting one of its own.
    { "ir_tx",     "IR send",   "940nm LED",  BUS_PWM,     CAT_WIRELESS, M(kPinsIrTx),     true,  0x00 },
    { "gps",       "GPS",       "NEO-M8N",    BUS_UART,    CAT_SENSORS,  M(kPinsGps),      false, 0x00 },
    { "dht11",     "Climate",   "DHT11/22",   BUS_ONEWIRE, CAT_SENSORS,  M(kPinsDht),      false, 0x00 },
    { "battery",   "Battery",   "divider",    BUS_ADC,     CAT_SENSORS,  M(kPinsBattery),  false, 0x00 },
    { "rtc",       "Clock",     "DS3231",     BUS_I2C,     CAT_SENSORS,  M(kPinsI2C),      true,  0x68 },
    { "sdcard",    "SD Card",   "microSD",    BUS_SPI,     CAT_TOOLS,    M(kPinsSd),       true,  0x00 },
    { "buzzer",    "Buzzer",    "piezo",      BUS_PWM,     CAT_TESTING,  M(kPinsBuzzer),   true,  0x00 },
    { "vibration", "Vibration", "motor",      BUS_GPIO,    CAT_TESTING,  M(kPinsVibe),     true,  0x00 },
    { "led",       "Alert LED", "LED",        BUS_GPIO,    CAT_TESTING,  M(kPinsLed),      true,  0x00 },
    { "ibutton",   "iButton",   "DS1990",     BUS_ONEWIRE, CAT_TESTING,  M(kPinsIbutton),  true,  0x00 },
};

#define MODULE_COUNT (sizeof(kModules) / sizeof(kModules[0]))

const Module *modules(void)      { return kModules; }
unsigned      module_count(void) { return MODULE_COUNT; }

const Module *module_by_id(const char *id) {
    if (!id) return nullptr;
    for (unsigned i = 0; i < MODULE_COUNT; i++)
        if (nova::ieq(id, kModules[i].id)) return &kModules[i];
    return nullptr;
}

bool module_wired(const Module &m) {
    // Nothing to wire is wired. The radio is on the board.
    if (m.bus == BUS_NONE) return true;
    for (unsigned i = 0; i < m.npins; i++)
        if (board::pin(m.pins[i]) == board::PIN_NONE) return false;
    return true;
}

// --- what the last scan found -------------------------------------------------

static uint8_t g_presence[MODULE_COUNT];

static int index_of(const Module &m) {
    int i = (int)(&m - kModules);
    return (i >= 0 && i < (int)MODULE_COUNT) ? i : -1;
}

Presence module_presence(const Module &m) {
    if (!module_wired(m)) return MOD_UNWIRED;
    int i = index_of(m);
    return i < 0 ? MOD_UNKNOWN : (Presence)g_presence[i];
}

void module_set_presence(const Module &m, Presence p) {
    int i = index_of(m);
    if (i >= 0) g_presence[i] = (uint8_t)p;
}

// --- probing ------------------------------------------------------------------
//
// Only the modules that can actually be ASKED are asked. A buzzer cannot answer
// a question — there is no way to tell a wired one from an unwired pin — so it
// reports as present when its pin is assigned and that is the honest limit of
// what can be known without making a noise.

static bool i2c_present(uint8_t addr) {
    int sda = board::pin(board::PIN_SDA);
    int scl = board::pin(board::PIN_SCL);
    if (sda == board::PIN_NONE || scl == board::PIN_NONE) return false;
    unsigned bus = ((sda / 2) % 2) ? 1u : 0u;
    // The display has already brought this bus up at 1 MHz. Re-initialising it
    // here would reset that; asking on it does not.
    uint8_t probe = 0;
    return fw_i2c_write(bus, addr, &probe, 1, 0) >= 0;
}

void modules_scan(void) {
    for (unsigned i = 0; i < MODULE_COUNT; i++) {
        const Module &m = kModules[i];
        if (!module_wired(m)) { g_presence[i] = MOD_UNWIRED; continue; }

        switch (m.bus) {
            case BUS_NONE:
                // The radio is there if the firmware has one. fw_net_connected
                // says nothing about whether the chip exists, but a board with
                // no radio reserves no pins for one — which fw_gpio_usable does
                // report.
                g_presence[i] = fw_gpio_usable(23) ? MOD_ABSENT : MOD_PRESENT;
                break;

            case BUS_I2C:
                g_presence[i] = i2c_present(m.addr) ? MOD_PRESENT : MOD_ABSENT;
                break;

            default:
                // SPI, UART and the rest need their own driver to say anything
                // useful, and those drivers are not written yet. Reporting
                // MOD_UNKNOWN rather than guessing keeps the Hardware screen
                // honest: it says "not checked", which is true, instead of
                // "absent", which would be a claim.
                g_presence[i] = MOD_UNKNOWN;
                break;
        }
    }
}

}  // namespace nova
