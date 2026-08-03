// I2CScan — sweep the bus and name what answers. Converted from v1's I2CScan.
//
// The scan is one transaction per address: a one-byte read either gets an
// acknowledge or does not, which is the whole protocol for "is anything there".
// v1 used MicroPython's i2c.scan(); fw_i2c_read returning the SDK's byte count
// straight through is what makes the same thing possible here.
#include "rpc_app.h"

RPC_APP_VER("i2cscan", "2.0");

// The device table v1 carried, because an address on its own tells you almost
// nothing. Addresses that several parts share list them together rather than
// guessing at one — a wrong name is worse than no name when someone is trying
// to work out what is wired up.
struct Known { unsigned char addr; const char *what; };
static const Known kKnown[] = {
    { 0x0C, "HMC5883L / QMC5883L compass" },
    { 0x18, "MCP9808 temperature, or LIS3DH accelerometer" },
    { 0x1E, "HMC5883L compass" },
    { 0x20, "PCF8574 or MCP23017 I/O expander" },
    { 0x23, "BH1750 light sensor" },
    { 0x27, "PCF8574 I/O expander (common LCD backpack)" },
    { 0x29, "TSL2561 light, or VL53L0X range" },
    { 0x38, "AHT10 / AHT20 temperature and humidity" },
    { 0x39, "TSL2561 light sensor" },
    { 0x3C, "SSD1306 / SH1106 OLED" },
    { 0x3D, "SSD1306 OLED (alternate)" },
    { 0x40, "HTU21D / Si7021 humidity, or INA219 current" },
    { 0x44, "SHT30 / SHT31 temperature and humidity" },
    { 0x48, "ADS1115 ADC, or LM75 temperature" },
    { 0x50, "AT24C EEPROM" },
    { 0x53, "ADXL345 accelerometer" },
    { 0x57, "MAX30102 pulse sensor, or AT24C EEPROM" },
    { 0x5A, "MLX90614 infrared thermometer, or CCS811 gas" },
    { 0x68, "DS1307 / DS3231 clock, or MPU6050 IMU" },
    { 0x69, "MPU6050 IMU (alternate)" },
    { 0x76, "BMP280 / BME280 pressure and humidity" },
    { 0x77, "BMP180 / BMP280 / BME280 (alternate)" },
};

static const char *identify(unsigned char addr) {
    for (unsigned i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); i++)
        if (kKnown[i].addr == addr) return kKnown[i].what;
    return 0;
}

static bool streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static unsigned parse_uint(const char *s, unsigned fallback) {
    if (!s || !*s) return fallback;
    unsigned v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return fallback;
        v = v * 10 + (unsigned)(*s - '0');
    }
    return v;
}

static int i2cscan_cmd(int argc, char **argv) {
    unsigned bus = 0, sda = 4, scl = 5, baud = 100000;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            fw_printf("Usage: i2cscan [bus] [sda] [scl] [baud]\n");
            fw_printf("  Defaults: bus 0, SDA 4, SCL 5, 100000 baud.\n");
            fw_printf("  Reports every address that acknowledges, and names it\n");
            fw_printf("  where the address is one a common part uses.\n");
            return 0;
        }
    }
    if (argc > 1) bus  = parse_uint(argv[1], 0);
    if (argc > 2) sda  = parse_uint(argv[2], 4);
    if (argc > 3) scl  = parse_uint(argv[3], 5);
    if (argc > 4) baud = parse_uint(argv[4], 100000);

    if (bus > 1) {
        fw_printf("Bus must be 0 or 1.\n");
        return 1;
    }
    if (!fw_gpio_usable(sda) || !fw_gpio_usable(scl)) {
        fw_printf("GPIO %u/%u cannot be used here.\n", sda, scl);
        fw_printf("  This board has %u pins, and reserves the ones the radio uses.\n",
                  fw_gpio_count());
        return 1;
    }

    if (fw_i2c_init(bus, sda, scl, baud) != 0) {
        fw_printf("Could not bring up I2C%u on SDA %u / SCL %u.\n", bus, sda, scl);
        return 1;
    }

    fw_printf("Scanning I2C%u  (SDA %u, SCL %u, %u Hz)\n", bus, sda, scl, baud);

    // 0x00-0x07 and 0x78-0x7F are reserved by the I2C specification and are not
    // device addresses, so probing them would only produce noise.
    int found = 0;
    for (unsigned a = 0x08; a <= 0x77; a++) {
        // A ONE-byte read, not a zero-length write. i2c_write_blocking with a
        // length of zero returns success without generating a transaction at
        // all, so every address appeared to answer — this reported 112 devices
        // on a bus with nothing attached to it.
        unsigned char rx = 0;
        int r = fw_i2c_read(bus, a, &rx, 1, 0);
        if (r < 1) continue;                       // no acknowledge: nothing there

        const char *what = identify((unsigned char)a);
        if (what) fw_printf("  0x%02X   %s\n", a, what);
        else      fw_printf("  0x%02X   (unrecognised)\n", a);
        found++;

        if (fw_task_should_stop()) break;
    }

    fw_i2c_deinit(bus);

    if (!found) {
        fw_printf("Nothing answered.\n");
        fw_printf("  Check the wiring, that both lines are pulled up, and that\n");
        fw_printf("  SDA and SCL are the right way round.\n");
        return 1;
    }
    fw_printf("%d device%s found.\n", found, found == 1 ? "" : "s");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("i2cscan", "scan the I2C bus and name what answers", i2cscan_cmd);
    return 0;
}
