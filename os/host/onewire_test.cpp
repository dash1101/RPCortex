// The 1-Wire CRC, and what it protects against.
//
// A ROM read has no acknowledgement and no retry: sixty-four bits arrive off a
// bit-banged pin and either they are the key's registration number or they are
// noise shaped like one. The CRC is the only thing standing between those two,
// so the cases that matter here are the ones where a reader without it would
// report a confident answer — an untouched pin, a single flipped bit, and two
// bytes that swapped places.
#include "../core/onewire.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %-44s got '%s', want '%s'\n", what, got, want);
        fails++;
    }
}

int main(void) {
    printf("onewire_test - the Dallas CRC and the family byte\n");

    // A DS18B20 registration number from the Maxim CRC worked example, which is
    // the usual reference vector for this polynomial: family 28, and the CRC
    // over the first seven bytes comes out as the eighth.
    const uint8_t ds18b20[OW_ROM_BYTES] =
        { 0x28, 0x1D, 0x39, 0x31, 0x02, 0x00, 0x00, 0xF0 };

    // --- the checksum itself ---------------------------------------------------
    ck(ow_crc8(ds18b20, 7) == 0xF0, "the CRC over the first 56 bits is the eighth byte");
    ck(ow_crc8(ds18b20, OW_ROM_BYTES) == 0, "and running it over all eight leaves zero");
    ck(ow_crc8(nullptr, 8) == 0, "no data does not crash");
    ck(ow_crc8(ds18b20, 0) == 0, "and an empty run is zero");

    // --- a whole registration number -------------------------------------------
    ck(ow_rom_ok(ds18b20), "a good ROM checks out");
    {
        // Every single-bit flip, which is what a marginal pull-up produces.
        int caught = 0;
        for (int byte = 0; byte < OW_ROM_BYTES; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                uint8_t rom[OW_ROM_BYTES];
                memcpy(rom, ds18b20, sizeof(rom));
                rom[byte] ^= (uint8_t)(1u << bit);
                if (!ow_rom_ok(rom)) caught++;
            }
        }
        ck(caught == 64, "every one of the 64 single-bit errors is caught");
    }
    {
        // Two bytes transposed. A plain sum would not notice; a CRC does.
        uint8_t rom[OW_ROM_BYTES];
        memcpy(rom, ds18b20, sizeof(rom));
        const uint8_t t = rom[1]; rom[1] = rom[2]; rom[2] = t;
        ck(!ow_rom_ok(rom), "two swapped bytes are caught");
    }
    {
        // The two a floating or held pin produces. All-zero passes the CRC on
        // its own, which is exactly why ow_rom_ok does not stop at the CRC.
        uint8_t zero[OW_ROM_BYTES];
        memset(zero, 0x00, sizeof(zero));
        ck(ow_crc8(zero, OW_ROM_BYTES) == 0, "an all-zero ROM passes the CRC");
        ck(!ow_rom_ok(zero), "but is refused anyway - that is an untouched pin");
        uint8_t ones[OW_ROM_BYTES];
        memset(ones, 0xFF, sizeof(ones));
        ck(!ow_rom_ok(ones), "and so is all-ones, which is an idle line");
    }
    ck(!ow_rom_ok(nullptr), "no ROM at all is not a ROM");

    // --- the family byte ---------------------------------------------------------
    eq(ow_family_name(0x01), "DS1990A / DS2401", "01 is the iButton");
    eq(ow_family_name(0x28), "DS18B20 thermometer", "28 is a DS18B20");
    eq(ow_family_name(0x10), "DS18S20 thermometer", "10 is a DS18S20");
    eq(ow_family_name(0x22), "DS1822 thermometer", "22 is a DS1822");
    eq(ow_family_name(0x99), "unknown family", "an unlisted code is not guessed at");
    eq(ow_family_name(0x00), "unknown family", "and neither is zero");
    {
        // The iButton's own name has to fit beside the family byte on the Nova
        // D1's 20-column body, because that is the screen it exists for: the
        // row reads "01  <name>", so the name has sixteen characters.
        //
        // Only this one is held to it. A DS18B20 touched to an iButton contact
        // is a real thing to do and its name is longer, but text_fit ellipsises
        // rather than overflowing, and "DS18B20 thermom.." on a screen about
        // iButtons is a perfectly good answer. Shortening every name to suit a
        // panel would cost the shell, which is the primary reader, the part
        // number that is the actual information.
        ck(strlen(ow_family_name(0x01)) <= 16,
           "the iButton family name fits beside its byte on the panel");
        ck(strlen(ow_family_name(0x99)) <= 16, "and so does the unknown one");
    }

    // --- the timing table ----------------------------------------------------------
    //
    // Not arithmetic, but worth pinning: these are the AN126 standard-speed
    // numbers, and a tidy-up that "rounded" one would be a protocol change with
    // no visible symptom until a key stopped reading.
    ck(OW_T_H == 480 && OW_T_I == 70 && OW_T_J == 410, "the reset slot is 480/70/410");
    ck(OW_T_A == 6 && OW_T_B == 64, "a write-1 is 6 low then 64 released");
    ck(OW_T_C == 60 && OW_T_D == 10, "a write-0 is 60 low then 10 released");
    ck(OW_T_E == 9 && OW_T_F == 55, "a read samples 9 after the 6, then waits 55");
    ck(OW_T_A + OW_T_B == 70 && OW_T_C + OW_T_D == 70 && OW_T_A + OW_T_E + OW_T_F == 70,
       "and all three bit slots are the same 70 us long");
    ck(OW_CMD_READ_ROM == 0x33, "Read ROM is 33h");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
