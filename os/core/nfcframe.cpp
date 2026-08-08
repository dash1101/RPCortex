// The PN532's frame format. See nfcframe.h for what each piece is grounded in.
#include "nfcframe.h"

#include <stdio.h>
#include <string.h>

uint32_t pn532_frame(const uint8_t *body, uint32_t n, uint8_t *out, uint32_t cap) {
    if (!body || !out) return 0;
    // LEN is one byte, and a data field of zero would be an ACK rather than an
    // information frame. Neither is something this driver builds.
    if (n == 0 || n > 255) return 0;
    if (cap < n + 7) return 0;

    uint32_t at = 0;
    out[at++] = 0x00;                       // preamble
    out[at++] = 0x00;                       // start code, first byte
    out[at++] = 0xFF;                       // start code, second byte
    out[at++] = (uint8_t)n;                 // LEN
    out[at++] = (uint8_t)(0x100 - n);       // LCS: lower byte of LEN+LCS is 0

    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        out[at++] = body[i];
        sum = (uint8_t)(sum + body[i]);
    }
    out[at++] = (uint8_t)(0x100 - sum);     // DCS
    out[at++] = 0x00;                       // postamble
    return at;
}

bool pn532_is_ack(const uint8_t *buf, uint32_t len) {
    // Start code, then the ACK's LEN 0x00 and LCS 0xFF. The preamble and
    // postamble around them are not required — § 7.2.9 lets the chip drop both
    // — and LEN 0x00 belongs to no information frame, so these four bytes
    // identify an ACK on their own.
    static const uint8_t kAck[4] = { 0x00, 0xFF, 0x00, 0xFF };
    if (!buf || len < sizeof(kAck)) return false;
    for (uint32_t i = 0; i + sizeof(kAck) <= len; i++)
        if (memcmp(buf + i, kAck, sizeof(kAck)) == 0) return true;
    return false;
}

int pn532_response(const uint8_t *buf, uint32_t len, uint8_t cmd,
                   const uint8_t **payload) {
    if (payload) *payload = nullptr;
    if (!buf) return -1;

    // Anchor on the START CODE, 00 FF, and not on the preamble.
    //
    // The preamble is optional. UM0701-02 § 7.2.9 lets the chip stop sending
    // the preamble and the postamble to save bus time, and the worked examples
    // beside § 6.2.1.1 make the point explicitly: several frames written as
    // "xx xx xx xx 00 FF 02 FE D4 02 2A", with arbitrary filler in front, are
    // all said to be the same frame. Requiring 00 00 FF would read those as
    // noise. Over I2C there is filler in front regardless — the § 6.2.4 status
    // byte comes first on every read.
    //
    // Everything that is not this command's answer is stepped over rather than
    // treated as an error: an ACK frame, an application-level error frame
    // (§ 6.2.1.5, TFI 0x7F) and a leftover reply to something else can all be
    // sitting in front of the one that was asked for.
    for (uint32_t i = 0; i + 2 <= len; i++) {
        if (buf[i] != 0x00 || buf[i + 1] != 0xFF) continue;
        if (i + 4 > len) break;                       // no room for LEN and LCS

        const uint8_t n = buf[i + 2];
        const uint8_t lcs = buf[i + 3];
        if ((uint8_t)(n + lcs) != 0x00) continue;     // § 6.2.1.1 length checksum
        if (n < 2) continue;                          // TFI and a code, at least
        // The data field plus its checksum has to be entirely present. A short
        // read that stopped mid-frame is the case this rejects, and it is the
        // one that would otherwise produce a UID made of stale buffer.
        if ((uint32_t)i + 4 + n + 1 > len) continue;

        if (buf[i + 4] != PN532_TFI_CHIP) continue;   // not from the chip
        if (buf[i + 5] != (uint8_t)(cmd + 1)) continue;  // answer to something else

        uint8_t sum = 0;
        for (uint32_t k = 0; k < n; k++) sum = (uint8_t)(sum + buf[i + 4 + k]);
        if ((uint8_t)(sum + buf[i + 4 + n]) != 0x00) continue;   // data checksum

        if (payload) *payload = buf + i + 6;
        return (int)n - 2;                            // minus TFI and the code
    }
    return -1;
}

bool pn532_version(const uint8_t *payload, uint32_t len,
                   uint8_t *ic, uint8_t *ver, uint8_t *rev) {
    // IC, Ver, Rev, Support. Support is not read: it is a bitmap of which card
    // families the firmware handles and nothing here asks that question.
    if (!payload || len < 4) return false;
    if (ic)  *ic  = payload[0];
    if (ver) *ver = payload[1];
    if (rev) *rev = payload[2];
    return true;
}

int pn532_target(const uint8_t *payload, uint32_t len, NfcTarget *out) {
    if (!payload || len < 1) return -1;

    const uint8_t nbtg = payload[0];
    if (nbtg == 0) return 0;                 // the antenna was empty, which is fine

    // Tg, SENS_RES(2), SEL_RES, NFCIDLength — six bytes before the UID starts.
    if (len < 6) return -1;
    const uint8_t uid_len = payload[5];
    // NFCID1 is 4, 7 or 10 bytes for type A. The bound is what stops a corrupt
    // length walking off the end of the payload and into whatever follows.
    if (uid_len == 0 || uid_len > 10) return -1;
    if (len < (uint32_t)6 + uid_len) return -1;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->tg      = payload[0 + 1];
        out->atqa[0] = payload[2];
        out->atqa[1] = payload[3];
        out->sak     = payload[4];
        out->uid_len = uid_len;
        memcpy(out->uid, payload + 6, uid_len);
    }
    return (int)nbtg;
}

const char *nfc_type_name(uint8_t sak, const uint8_t *atqa) {
    // SAK values from the MicroPython suite's _CLASSIC_BY_SAK, which is what has
    // been read off real cards with this reader.
    switch (sak) {
        case 0x08: return "MIFARE Classic 1K";
        case 0x88: return "MIFARE Classic 1K";     // Infineon-made, same layout
        case 0x18: return "MIFARE Classic 4K";
        case 0x98: return "MIFARE Classic 4K";
        case 0x09: return "MIFARE Mini";
        default:   break;
    }
    // SAK 0x00 with 0x44 somewhere in the ATQA is the NTAG / Ultralight family.
    // The exact part needs GET_VERSION over InDataExchange, which this driver
    // does not send, so the family is as far as it can honestly go.
    if (sak == 0x00 && atqa && (atqa[0] == 0x44 || atqa[1] == 0x44))
        return "NTAG / Ultralight";
    return "ISO14443-3A";
}

uint32_t nfc_hex(const uint8_t *data, uint32_t n, char *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!data) return 0;
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        // Three characters a byte after the first, and one for the terminator.
        if (at + (at ? 3u : 2u) + 1u > cap) break;
        int w = snprintf(out + at, cap - at, "%s%02X", at ? " " : "", data[i]);
        if (w <= 0) break;
        at += (uint32_t)w;
    }
    return at;
}
