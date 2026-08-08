// The PN532's frame format, and what an ISO14443-A tag turns out to be.
//
// Everything here is arithmetic over a byte buffer, which is deliberate: the
// bus transaction is the part that needs a chip on the end of it, and the frame
// checking is the part that goes wrong quietly. A response frame whose length
// checksum is not verified will happily hand back a UID assembled out of
// whatever was left in the receive buffer, and it looks exactly like a card.
//
// So the framing lives here, away from the I2C, where it can be given a
// deliberately broken frame and checked. Same split as btname.h and for the
// same reason.
//
// Grounded in the NXP PN532 User Manual, UM0701-02 Rev. 02 (5 November 2007),
// with the section numbers quoted at each piece. Cross-checked against NXP
// AN10609_3 "PN532 C106 application note" Rev. 1.2 (14 January 2010), which
// carries the same frame drawing and a worked InListPassiveTarget exchange with
// real byte values in it.
#ifndef RPC_NFCFRAME_H
#define RPC_NFCFRAME_H

#include <stdint.h>

// The 7-bit I2C address. UM0701-02 § 6.2.4 gives the address as 0x48, which is
// the 8-bit form with the read/write bit in it; the RP2 I2C block wants the
// 7-bit form, so 0x48 >> 1 = 0x24. That is the address novamodtab already
// probes for, and the two must not drift apart.
#define PN532_I2C_ADDR   0x24

// UM0701-02 § 6.2.4: "able to communicate with a host controller in fast mode
// (up to 400 KHz CLK)". The Nova D1 panel shares this bus and runs it at 1 MHz,
// which is above what the PN532 is specified for — see the note in nfc.cpp.
#define PN532_I2C_MAX_HZ 400000

// Command codes, UM0701-02 chapter 7 (and the same table in AN10609_3 § 3.2.1).
#define PN532_CMD_FIRMWARE  0x02   // GetFirmwareVersion,   § 7.2.2,  p: 73
#define PN532_CMD_SAMCONFIG 0x14   // SAMConfiguration,     § 7.2.10, p: 89
#define PN532_CMD_LISTTARGET 0x4A  // InListPassiveTarget,  § 7.3.5,  p: 115

// The frame identifier byte, UM0701-02 § 6.2.1.1: 0xD4 on the way to the chip,
// 0xD5 on the way back.
#define PN532_TFI_HOST 0xD4
#define PN532_TFI_CHIP 0xD5

// Longest command this driver sends is SAMConfiguration at four body bytes;
// seven bytes of framing on top of that. Rounded up so a caller does not have
// to think about it.
#define PN532_FRAME_MAX 32

// Build a normal information frame around `body` (which starts with the TFI).
//
// UM0701-02 § 6.2.1.1:
//
//   00  00 FF  LEN  LCS  TFI PD0..PDn  DCS  00
//   ^   ^      ^    ^    ^             ^    ^
//   |   |      |    |    |             |    postamble
//   |   |      |    |    data, LEN bytes, first is the command code
//   |   |      |    lower byte of [LEN + LCS] is 00
//   |   |      number of bytes in the data field
//   |   start code
//   preamble
//
// and the data checksum: lower byte of [TFI + PD0 + ... + PDn + DCS] is 00.
//
// Returns the number of bytes written, or 0 if it would not fit.
uint32_t pn532_frame(const uint8_t *body, uint32_t n, uint8_t *out, uint32_t cap);

// Whether `buf` contains the ACK frame, UM0701-02 § 6.2.1.3 Fig 15:
// 00 00 FF 00 FF 00. Searched for rather than matched at offset zero, because
// over I2C the chip prepends a status byte (§ 6.2.4) and the read length is
// fixed by the caller, so the ACK arrives with a byte in front of it and
// padding behind.
bool pn532_is_ack(const uint8_t *buf, uint32_t len);

// A note that applies to both of the functions here: the PREAMBLE AND POSTAMBLE
// ARE OPTIONAL. UM0701-02 § 7.2.9 lets the chip stop sending them, and the
// examples beside § 6.2.1.1 show the same GetFirmwareVersion frame written both
// with filler in front and starting flat at the 00 FF start code. So both scan
// for the start code, not for the preamble; a frame missing its postamble is
// complete once its data checksum is in.

// Find the response to `cmd` in `buf` and hand back its payload.
//
// A response carries the command code plus one (§ 6.2.1.1: the chip answers
// GetFirmwareVersion 0x02 with 0xD5 0x03), and everything is checked before
// anything is believed: both checksums, the frame identifier, and that the
// whole data field is actually present in the buffer.
//
// `payload` is set to the bytes AFTER the response code, and the return value
// is how many there are. -1 when no valid response to that command is in the
// buffer. A payload of zero bytes is a legal answer, so the return value is
// what distinguishes "nothing there" from "nothing to say".
//
// Extended information frames (§ 6.2.1.2, for data fields over 255 bytes) are
// not handled and cannot be mistaken for a normal one: they carry LEN 0xFF with
// LCS 0xFF, whose lower byte sums to 0xFE rather than 0x00, so the length
// checksum rejects them. Nothing this driver asks for comes back that long.
int pn532_response(const uint8_t *buf, uint32_t len, uint8_t cmd,
                   const uint8_t **payload);

// GetFirmwareVersion's answer, UM0701-02 § 7.2.2 p: 73 —
// "Output: D5 03 IC Ver Rev Support", where IC is 0x32 for a PN532.
bool pn532_version(const uint8_t *payload, uint32_t len,
                   uint8_t *ic, uint8_t *ver, uint8_t *rev);

// One card, as the anticollision reported it.
struct NfcTarget {
    uint8_t tg;          // the chip's logical target number, 1-based
    uint8_t atqa[2];     // SENS_RES, in the order the PN532 sends it
    uint8_t sak;         // SEL_RES
    uint8_t uid_len;     // NFCIDLength, 4 / 7 / 10 in practice
    uint8_t uid[10];     // NFCID1
};

// Parse an InListPassiveTarget answer for 106 kbps type A.
//
// UM0701-02 § 7.3.5 p: 115 gives the per-target layout as
//
//   Tg  SENS_RES (2 bytes)  SEL_RES (1 byte)  NFCIDLength (1)  NFCID1[]  [ATS[]]
//
// preceded by NbTg, the number of targets. AN10609_3 Table 2 shows a real
// two-card exchange in the same shape, which is what this was written against.
//
// Returns the number of targets the chip said it found — 0 is the ordinary
// "nothing was on the antenna" answer — or -1 if the buffer does not hold a
// well-formed first target. Only the first is parsed out; MaxTg is 1 in every
// call this driver makes.
int pn532_target(const uint8_t *payload, uint32_t len, NfcTarget *out);

// What kind of card that is, from SAK and ATQA.
//
// Ported from the MicroPython suite's novanfc.identify(), which shipped, and
// kept conservative in the same way: anything unrecognised is reported as plain
// ISO14443-3A rather than guessed at, because the UID is captured either way
// and a wrong name is worse than no name.
//
// THE ATQA TEST IS ORDER-AGNOSTIC ON PURPOSE. AN10609_3 Table 2 shows the
// PN532's own order — a Mifare Classic answers SENS_RES 00 04 and an Ultralight
// answers 44 00 — which is the reverse of the way a Flipper prints ATQA. The v1
// comment recorded that this had never been confirmed against a device; this
// note is that document, and the membership test is kept anyway because a
// Classic ATQA contains no 0x44 and is caught by SAK first, so nothing is lost
// by not depending on the order.
const char *nfc_type_name(uint8_t sak, const uint8_t *atqa);

// Bytes as "04 A2 B3 C4", upper case, space separated. Always terminated.
// Returns the length written.
uint32_t nfc_hex(const uint8_t *data, uint32_t n, char *out, uint32_t cap);

#endif  // RPC_NFCFRAME_H
