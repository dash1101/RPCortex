// Dallas 1-Wire: the checksum, the family codes, and the master's timings.
//
// The bit-banging itself needs a pin and cannot be tested without one, so it
// lives in shell/ibutton.cpp. What is here is everything that can be checked on
// a host: the CRC that says whether the 64 bits arrived intact, what the family
// byte means, and the timing table the bit-banger is built from — as named
// constants, so the numbers appear once and the driver cannot drift from the
// document they came out of.
//
// Timings are the standard-speed column of Table 2 in the Maxim / Analog
// Devices application note AN126, "1-Wire Communication Through Software"
// (30 May 2002). Table 1 of the same note gives the four operations those
// letters are used by, and they are quoted beside each one below.
//
// The CRC and the ROM layout are from the DS1990A data sheet (Maxim / Analog
// Devices): a 64-bit registration number made of an 8-bit family code, a 48-bit
// serial number and an 8-bit CRC over the first 56 bits, generated with the
// polynomial X^8 + X^5 + X^4 + 1.
#ifndef RPC_ONEWIRE_H
#define RPC_ONEWIRE_H

#include <stdint.h>

// ROM commands. The DS1990A data sheet gives Read ROM as 33h, and notes that
// 0Fh does the same thing for compatibility with the original DS1990.
#define OW_CMD_READ_ROM 0x33

// A registration number is eight bytes: family, six of serial, CRC.
#define OW_ROM_BYTES 8

// AN126 Table 2, standard speed, in microseconds.
//
// AN126 Table 1, what each is used by:
//   Write 1   drive low, delay A;  release, delay B
//   Write 0   drive low, delay C;  release, delay D
//   Read      drive low, delay A;  release, delay E;  sample;  delay F
//   Reset     delay G;  drive low, delay H;  release, delay I;
//             sample (0 = device present);  delay J
#define OW_T_A   6
#define OW_T_B  64
#define OW_T_C  60
#define OW_T_D  10
#define OW_T_E   9
#define OW_T_F  55
#define OW_T_G   0
#define OW_T_H 480
#define OW_T_I  70
#define OW_T_J 410

// The Dallas / Maxim CRC-8 over `n` bytes.
//
// The polynomial X^8 + X^5 + X^4 + 1 shifted right is 0x8C, which is what makes
// this the familiar reflected loop rather than the textbook one. Feeding it all
// eight ROM bytes gives zero when they are intact, which is the cheaper of the
// two ways to check and the one ow_rom_ok uses.
uint8_t ow_crc8(const uint8_t *data, uint32_t n);

// Whether a 64-bit registration number checks out.
//
// False for an all-zero or all-ones ROM as well as for a bad CRC. Those two are
// what a floating pin reads back as, and both happen to pass an ordinary CRC
// test — 0x00 x8 has a CRC of zero — so a reader without this guard reports a
// confident ROM of 00 00 00 00 00 00 00 00 for a key that was never touched to
// it. That is the silent failure this function exists for.
bool ow_rom_ok(const uint8_t *rom);

// What the family byte says the part is, or "unknown family" for one that is
// not in the short list below. Deliberately short: only the codes that are
// well established are named, because a wrong part name reads as fact.
const char *ow_family_name(uint8_t family);

#endif  // RPC_ONEWIRE_H
