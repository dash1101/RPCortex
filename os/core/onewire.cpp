// Dallas 1-Wire arithmetic. See onewire.h for what each piece is grounded in.
#include "onewire.h"

uint8_t ow_crc8(const uint8_t *data, uint32_t n) {
    uint8_t crc = 0;
    if (!data) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        for (int bit = 0; bit < 8; bit++) {
            const uint8_t mix = (uint8_t)((crc ^ b) & 0x01);
            crc = (uint8_t)(crc >> 1);
            if (mix) crc ^= 0x8C;       // X^8 + X^5 + X^4 + 1, reflected
            b = (uint8_t)(b >> 1);
        }
    }
    return crc;
}

bool ow_rom_ok(const uint8_t *rom) {
    if (!rom) return false;

    // A line that nothing is driving reads as all ones; a line held down reads
    // as all zeros. Both are what an absent key looks like, and both would
    // otherwise be reported as a perfectly good registration number — 00 x8
    // has a CRC of 00, so the checksum agrees with it.
    bool all_zero = true, all_ones = true;
    for (uint32_t i = 0; i < OW_ROM_BYTES; i++) {
        if (rom[i] != 0x00) all_zero = false;
        if (rom[i] != 0xFF) all_ones = false;
    }
    if (all_zero || all_ones) return false;

    // Running the CRC over the checksum byte as well leaves zero when the whole
    // registration number is intact.
    return ow_crc8(rom, OW_ROM_BYTES) == 0;
}

const char *ow_family_name(uint8_t family) {
    switch (family) {
        // The one an iButton reader is for. Both parts carry the same silicon
        // serial number; the DS1990A is the blue steel can and the DS2401 is
        // the same thing in a TO-92.
        //
        // Kept SHORT, all of these. The Nova D1 panel is 21 characters wide and
        // this is printed with the family byte in front of it, so a longer name
        // is a name that gets cut off on the screen that most wants to show it.
        case 0x01: return "DS1990A / DS2401";
        case 0x10: return "DS18S20 thermometer";
        case 0x22: return "DS1822 thermometer";
        case 0x28: return "DS18B20 thermometer";
        default:   return "unknown family";
    }
}
