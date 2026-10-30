// The PN532's frames, built and taken apart.
//
// There is no reader on this desk, so the bus transaction cannot be tried at
// all — but the frames it carries are just bytes, and every way one can be
// wrong is reachable from here. That matters more than usual for this part: a
// response frame accepted without checking its length gives back a UID
// assembled out of stale buffer, and on the panel that is indistinguishable
// from a card that was really there.
//
// The well-formed frames below are not invented. The GetFirmwareVersion command
// frame is the one printed in UM0701-02 § 6.2.1.1, and the InListPassiveTarget
// exchange is the worked example in NXP AN10609_3 Table 2, byte for byte.
#include "../core/nfcframe.h"

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

// Wrap a chip-to-host payload in a valid frame, so the malformed cases below
// can each break exactly one thing about an otherwise good one.
static uint32_t reply(uint8_t code, const uint8_t *data, uint32_t n, uint8_t *out) {
    uint8_t body[64];
    body[0] = PN532_TFI_CHIP;
    body[1] = code;
    memcpy(body + 2, data, n);
    return pn532_frame(body, n + 2, out, 64);
}

int main(void) {
    printf("nfcframe_test - PN532 frames, and what a tag turns out to be\n");
    uint8_t f[64];
    char hex[64];

    // --- building ------------------------------------------------------------
    {
        // UM0701-02 § 6.2.1.1 prints this exact frame for GetFirmwareVersion:
        //   00 00 FF 02 FE D4 02 2A 00
        const uint8_t body[] = { PN532_TFI_HOST, PN532_CMD_FIRMWARE };
        const uint8_t want[] = { 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00 };
        uint32_t n = pn532_frame(body, sizeof(body), f, sizeof(f));
        ck(n == sizeof(want), "GetFirmwareVersion frames to nine bytes");
        ck(memcmp(f, want, sizeof(want)) == 0, "and matches the manual's example");
    }
    {
        // Every frame's own arithmetic, checked rather than eyeballed.
        const uint8_t body[] = { 0xD4, 0x4A, 0x01, 0x00 };
        uint32_t n = pn532_frame(body, sizeof(body), f, sizeof(f));
        ck(n == 11, "a four-byte body frames to eleven");
        ck((uint8_t)(f[3] + f[4]) == 0, "the length checksum sums to zero");
        uint8_t sum = 0;
        for (uint32_t i = 0; i < 4; i++) sum = (uint8_t)(sum + f[5 + i]);
        ck((uint8_t)(sum + f[9]) == 0, "and so does the data checksum");
        ck(f[10] == 0x00, "with a postamble on the end");
    }
    ck(pn532_frame(nullptr, 2, f, sizeof(f)) == 0, "a null body builds nothing");
    ck(pn532_frame(f, 0, f, sizeof(f)) == 0, "an empty body builds nothing");
    {
        const uint8_t body[] = { 0xD4, 0x02 };
        ck(pn532_frame(body, sizeof(body), f, 8) == 0, "a short buffer is refused");
    }

    // --- the ACK -------------------------------------------------------------
    {
        // UM0701-02 § 6.2.1.3 Fig 15.
        const uint8_t ack[] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
        ck(pn532_is_ack(ack, sizeof(ack)), "the ACK frame is recognised");
        // How it really arrives over I2C: the § 6.2.4 status byte in front, and
        // the rest of a fixed-length read behind.
        const uint8_t live[] = { 0x01, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00 };
        ck(pn532_is_ack(live, sizeof(live)), "and found behind the I2C status byte");
        const uint8_t nack[] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
        ck(!pn532_is_ack(nack, sizeof(nack)), "a NACK is not an ACK");
        ck(!pn532_is_ack(nullptr, 0), "no buffer is not an ACK");
        ck(!pn532_is_ack(live, 4), "and neither is half of one");
    }

    // --- finding a response ----------------------------------------------------
    const uint8_t ver_payload[] = { 0x32, 0x01, 0x06, 0x07 };   // IC, Ver, Rev, Support
    {
        uint32_t n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        const uint8_t *p = nullptr;
        ck(pn532_response(f, n, PN532_CMD_FIRMWARE, &p) == 4,
           "GetFirmwareVersion's four payload bytes are found");
        uint8_t ic = 0, v = 0, r = 0;
        ck(pn532_version(p, 4, &ic, &v, &r), "and read out");
        ck(ic == 0x32 && v == 1 && r == 6, "as a PN532 running v1.6");
    }
    {
        // The ACK the chip sends first sits in front of the answer, and so does
        // the status byte. Neither may be mistaken for the response.
        uint8_t buf[80];
        const uint8_t ack[] = { 0x01, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
        memcpy(buf, ack, sizeof(ack));
        uint32_t n = reply(0x03, ver_payload, sizeof(ver_payload), buf + sizeof(ack));
        const uint8_t *p = nullptr;
        ck(pn532_response(buf, sizeof(ack) + n, PN532_CMD_FIRMWARE, &p) == 4,
           "an ACK in front of the answer is stepped over");
        ck(p && p[0] == 0x32, "and the payload is still the right one");
    }
    {
        uint32_t n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        const uint8_t *p = nullptr;
        ck(pn532_response(f, n, PN532_CMD_LISTTARGET, &p) < 0,
           "a reply to another command is not accepted");
        ck(p == nullptr, "and hands back no payload");
    }
    {
        // Each checksum, broken on its own.
        uint32_t n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        f[4] ^= 0xFF;
        ck(pn532_response(f, n, PN532_CMD_FIRMWARE, nullptr) < 0,
           "a bad length checksum is rejected");
        n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        f[n - 2] ^= 0xFF;
        ck(pn532_response(f, n, PN532_CMD_FIRMWARE, nullptr) < 0,
           "a bad data checksum is rejected");
        n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        f[5] = PN532_TFI_HOST;
        ck(pn532_response(f, n, PN532_CMD_FIRMWARE, nullptr) < 0,
           "a frame claiming to be from the host is rejected");
    }
    {
        // The one that produces a plausible answer out of nothing: a read that
        // stopped part way, so LEN promises more than arrived. Cutting starts
        // at two, because losing only the postamble leaves a complete frame —
        // § 7.2.9 lets the chip omit it in the first place.
        uint32_t n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        ck(pn532_response(f, n - 1, PN532_CMD_FIRMWARE, nullptr) == 4,
           "a frame with no postamble is still a frame");
        for (uint32_t cut = 2; cut < 5; cut++)
            ck(pn532_response(f, n - cut, PN532_CMD_FIRMWARE, nullptr) < 0,
               "a truncated frame is rejected");
    }
    {
        // And with no preamble either, which is the other half of § 7.2.9. The
        // manual's own examples are written this way.
        const uint8_t bare[] = { 0x00, 0xFF, 0x06, 0xFA,
                                 0xD5, 0x03, 0x32, 0x01, 0x06, 0x07, 0xE8 };
        const uint8_t *p = nullptr;
        ck(pn532_response(bare, sizeof(bare), PN532_CMD_FIRMWARE, &p) == 4,
           "a frame with neither preamble nor postamble is accepted");
        ck(p && p[0] == 0x32 && p[1] == 0x01 && p[2] == 0x06,
           "and reads as a PN532 v1.6");
    }
    {
        const uint8_t junk[] = { 0xFF, 0xFF, 0x11, 0x22, 0x33 };
        ck(pn532_response(junk, sizeof(junk), PN532_CMD_FIRMWARE, nullptr) < 0,
           "noise is not a response");
        ck(pn532_response(nullptr, 0, PN532_CMD_FIRMWARE, nullptr) < 0,
           "and neither is nothing at all");
        // An extended frame (§ 6.2.1.2) must not be read as a normal one.
        const uint8_t ext[] = { 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x08, 0xF8,
                                0xD5, 0x03, 0x32, 0x01, 0x06, 0x07, 0x00, 0x00 };
        ck(pn532_response(ext, sizeof(ext), PN532_CMD_FIRMWARE, nullptr) < 0,
           "an extended frame is not mistaken for a normal one");
    }
    ck(!pn532_version(ver_payload, 3, nullptr, nullptr, nullptr),
       "a short version payload is refused");

    // --- the anticollision result ------------------------------------------------
    //
    // AN10609_3 Table 2: a real two-card exchange. Command 4A 02 00 answered
    // with NbTg 02, then a Mifare Classic and a Mifare Ultralight.
    const uint8_t two_cards[] = {
        0x02,
        0x01, 0x00, 0x04, 0x08, 0x04, 0x12, 0x67, 0x58, 0x32,
        0x02, 0x44, 0x00, 0x00, 0x08, 0x88, 0x04, 0xB6, 0xE4, 0x00, 0x00, 0x00, 0x00,
    };
    {
        NfcTarget t;
        ck(pn532_target(two_cards, sizeof(two_cards), &t) == 2,
           "the manual's example reports two targets");
        ck(t.tg == 0x01, "the first is target 1");
        ck(t.atqa[0] == 0x00 && t.atqa[1] == 0x04, "with ATQA 00 04");
        ck(t.sak == 0x08, "and SAK 08");
        ck(t.uid_len == 4, "a four-byte UID");
        nfc_hex(t.uid, t.uid_len, hex, sizeof(hex));
        eq(hex, "12 67 58 32", "which reads as the manual prints it");
        eq(nfc_type_name(t.sak, t.atqa), "MIFARE Classic 1K", "and is a Classic 1K");
    }
    {
        // The second card in the same example, fed in on its own — a seven-byte
        // UID, which is the shape that catches an off-by-one in the length.
        const uint8_t ntag[] = { 0x01, 0x01, 0x44, 0x00, 0x00, 0x07,
                                 0x04, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7 };
        NfcTarget t;
        ck(pn532_target(ntag, sizeof(ntag), &t) == 1, "one target is one target");
        ck(t.uid_len == 7, "a seven-byte UID is read whole");
        nfc_hex(t.uid, t.uid_len, hex, sizeof(hex));
        eq(hex, "04 A2 B3 C4 D5 E6 F7", "and printed whole");
        eq(nfc_type_name(t.sak, t.atqa), "NTAG / Ultralight", "SAK 00 with 0x44 is NTAG");
    }
    {
        NfcTarget t;
        const uint8_t none[] = { 0x00 };
        ck(pn532_target(none, sizeof(none), &t) == 0, "NbTg 0 is an empty antenna");
        ck(pn532_target(nullptr, 0, &t) < 0, "no payload is malformed");
        const uint8_t stub[] = { 0x01, 0x01, 0x00, 0x04, 0x08 };
        ck(pn532_target(stub, sizeof(stub), &t) < 0, "a target header cut short is refused");
        // The one that would read past the payload: a length longer than what
        // arrived. Silent on a device, and a UID full of whatever was next.
        const uint8_t lying[] = { 0x01, 0x01, 0x00, 0x04, 0x08, 0x0A, 0x11, 0x22 };
        ck(pn532_target(lying, sizeof(lying), &t) < 0, "a UID length past the end is refused");
        const uint8_t huge[] = { 0x01, 0x01, 0x00, 0x04, 0x08, 0xFF, 0x11, 0x22 };
        ck(pn532_target(huge, sizeof(huge), &t) < 0, "and so is one over ten bytes");
        const uint8_t zero[] = { 0x01, 0x01, 0x00, 0x04, 0x08, 0x00 };
        ck(pn532_target(zero, sizeof(zero), &t) < 0, "a zero-length UID is refused");
    }

    // --- InDataExchange ---------------------------------------------------------
    {
        // A Mifare read of block 4 to target 1, as a frame. The bytes are worked out from the
        // manual's frame layout (§ 6.2.1.1) rather than copied from a capture.
        uint8_t body[PN532_FRAME_MAX];
        const uint8_t read4[] = { 0x30, 0x04 };
        uint32_t nb = pn532_dataex_body(1, read4, sizeof(read4), body, sizeof(body));
        const uint8_t want_body[] = { 0xD4, 0x40, 0x01, 0x30, 0x04 };
        ck(nb == sizeof(want_body) && memcmp(body, want_body, nb) == 0, "InDataExchange's body is D4 40 Tg data");
        uint32_t n = pn532_frame(body, nb, f, sizeof(f));
        const uint8_t want[] = { 0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD4, 0x40, 0x01, 0x30, 0x04, 0xB7, 0x00 };
        ck(n == sizeof(want) && memcmp(f, want, n) == 0, "and frames to twelve bytes with the right checksums");

        // The longest it will carry fits the frame buffer it is sized for.
        uint8_t big[PN532_EXCHANGE_MAX];
        memset(big, 0xA5, sizeof(big));
        nb = pn532_dataex_body(1, big, sizeof(big), body, sizeof(body));
        ck(nb == PN532_EXCHANGE_MAX + 3, "a full-length exchange builds");
        uint8_t fb[PN532_FRAME_MAX];
        ck(pn532_frame(body, nb, fb, sizeof(fb)) == nb + 7, "and its frame fits PN532_FRAME_MAX");
        ck(pn532_dataex_body(1, big, PN532_EXCHANGE_MAX + 1, body, sizeof(body)) == 0, "one byte more is refused");
        ck(pn532_dataex_body(1, read4, 2, body, 4) == 0, "a short buffer is refused");
        ck(pn532_dataex_body(1, nullptr, 2, body, sizeof(body)) == 0, "a null payload with a length is refused");
        ck(pn532_dataex_body(1, nullptr, 0, body, sizeof(body)) == 3, "an empty exchange is the three-byte header");
        pn532_dataex_body(2, read4, 2, body, sizeof(body));
        ck(body[2] == 2, "and the target number is the caller's");
    }
    {
        // The answer: Status then the data. A Mifare block read comes back with sixteen bytes.
        uint8_t data[17];
        data[0] = 0x00;
        for (int i = 0; i < 16; i++) data[1 + i] = (uint8_t)(0xA0 + i);
        uint32_t n = reply(0x41, data, sizeof(data), f);
        const uint8_t *p = nullptr;
        int plen = pn532_response(f, n, PN532_CMD_DATAEX, &p);
        ck(plen == 17, "a block read's answer has seventeen payload bytes");
        uint8_t st = 0xEE;
        const uint8_t *d = nullptr;
        ck(pn532_dataex_result(p, (uint32_t)plen, &st, &d) == 16, "sixteen of them are data");
        ck(st == 0x00 && d && d[0] == 0xA0 && d[15] == 0xAF, "after a status of zero");

        // An authentication failure: just the status.
        const uint8_t autherr[] = { 0x14 };
        n = reply(0x41, autherr, 1, f);
        plen = pn532_response(f, n, PN532_CMD_DATAEX, &p);
        ck(plen == 1, "an authentication error is a one-byte answer");
        ck(pn532_dataex_result(p, (uint32_t)plen, &st, &d) == 0, "with no data behind it");
        ck(st == 0x14 && pn532_status_error(st) == 0x14, "and the status says 14");

        // The status bits other than the error code are kept and told apart.
        const uint8_t more[] = { 0x40 };
        n = reply(0x41, more, 1, f);
        plen = pn532_response(f, n, PN532_CMD_DATAEX, &p);
        pn532_dataex_result(p, (uint32_t)plen, &st, &d);
        ck(st == 0x40 && pn532_status_error(st) == 0, "MI is not an error");

        ck(pn532_dataex_result(nullptr, 0, &st, &d) == -1 && d == nullptr, "no payload is not an answer");
        ck(pn532_dataex_result(data, 0, &st, &d) == -1, "and neither is an empty one");
        // A reply to a different command is not a data exchange's answer.
        n = reply(0x03, ver_payload, sizeof(ver_payload), f);
        ck(pn532_response(f, n, PN532_CMD_DATAEX, nullptr) < 0, "a firmware version is not an exchange");
    }
    {
        // § 6.2.1.5: the chip's own "I could not read that".
        const uint8_t err[] = { 0x00, 0x00, 0xFF, 0x01, 0xFF, 0x7F, 0x81, 0x00 };
        ck(pn532_is_error_frame(err, sizeof(err)), "the application-level error frame is recognised");
        const uint8_t live[] = { 0x01, 0x00, 0x00, 0xFF, 0x01, 0xFF, 0x7F, 0x81, 0x00, 0x00 };
        ck(pn532_is_error_frame(live, sizeof(live)), "and found behind the I2C status byte");
        ck(pn532_response(live, sizeof(live), PN532_CMD_DATAEX, nullptr) < 0, "pn532_response does not read it as an answer");
        const uint8_t ack[] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
        ck(!pn532_is_error_frame(ack, sizeof(ack)), "an ACK is not an error frame");
        ck(!pn532_is_error_frame(err, 5), "half of one is not one");
        ck(!pn532_is_error_frame(nullptr, 0), "and nothing is not one");
    }

    // --- naming a card --------------------------------------------------------
    {
        const uint8_t classic[2] = { 0x00, 0x04 };
        const uint8_t ul[2]      = { 0x44, 0x00 };
        const uint8_t flipped[2] = { 0x00, 0x44 };     // the way a Flipper prints it
        eq(nfc_type_name(0x18, classic), "MIFARE Classic 4K", "SAK 18 is a 4K");
        eq(nfc_type_name(0x88, classic), "MIFARE Classic 1K", "SAK 88 is a 1K");
        eq(nfc_type_name(0x98, classic), "MIFARE Classic 4K", "SAK 98 is a 4K");
        eq(nfc_type_name(0x09, classic), "MIFARE Mini", "SAK 09 is a Mini");
        eq(nfc_type_name(0x00, ul), "NTAG / Ultralight", "SAK 00 with 44 00");
        eq(nfc_type_name(0x00, flipped), "NTAG / Ultralight",
           "and with the ATQA bytes the other way round");
        eq(nfc_type_name(0x00, classic), "ISO14443-3A",
           "SAK 00 with no 0x44 is not claimed to be an NTAG");
        eq(nfc_type_name(0x20, classic), "ISO14443-3A", "an unknown SAK is not guessed at");
        eq(nfc_type_name(0x00, nullptr), "ISO14443-3A", "and no ATQA at all does not crash");
    }

    // --- hex --------------------------------------------------------------------
    {
        const uint8_t b[] = { 0x04, 0x00, 0xFF };
        ck(nfc_hex(b, 3, hex, sizeof(hex)) == 8, "three bytes are eight characters");
        eq(hex, "04 00 FF", "upper case and space separated");
        char small[6];
        nfc_hex(b, 3, small, sizeof(small));
        eq(small, "04 00", "a short buffer stops on a byte boundary");
        char one[1];
        ck(nfc_hex(b, 3, one, sizeof(one)) == 0, "a buffer of one holds nothing");
        ck(one[0] == 0, "but is terminated");
        ck(nfc_hex(nullptr, 3, hex, sizeof(hex)) == 0, "and no data is no output");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
