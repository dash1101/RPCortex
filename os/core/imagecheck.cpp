#include "imagecheck.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);           // the staged image is in XIP flash; no alignment promises
    return v;
}

bool image_fits_chip(const uint8_t *img, size_t len, ImageChip chip,
                     uint32_t *sp, uint32_t *entry) {
    const bool rp2040 = chip == IMAGE_CHIP_RP2040;
    const size_t table = rp2040 ? 0x100 : 0;
    const uint32_t ram_top = rp2040 ? 0x20042000u : 0x20082000u;

    if (sp) *sp = 0;
    if (entry) *entry = 0;
    if (!img || len < table + 8) return false;

    uint32_t s = rd32(img + table), e = rd32(img + table + 4);
    if (sp) *sp = s;
    if (entry) *entry = e;

    // The stack starts at or below the top of this chip's SRAM, the entry point
    // is Thumb code in flash.
    if (s < 0x20000000u || s > ram_top) return false;
    if (e < 0x10000000u || e >= 0x11000000u || !(e & 1)) return false;

    if (!rp2040) {
        // Without its picobin block the RP2350 boot ROM will not run an image,
        // and an RP2040 image never has one.
        size_t scan = len < 4096 ? len : 4096;
        for (size_t off = 0; off + 4 <= scan; off += 4)
            if (rd32(img + off) == 0xffffded3u) return true;
        return false;
    }
    return true;
}

bool image_board(const uint8_t *img, size_t len, char *out, size_t cap) {
    if (!img || !out || cap < 2) return false;
    static const char m[] = IMAGE_MARKER;
    const size_t ml = sizeof(m) - 1;
    for (size_t i = 0; i + ml < len; i++) {
        if (img[i] != (uint8_t)m[0] || memcmp(img + i, m, ml) != 0) continue;
        size_t at = i + ml, n = 0;
        while (at + n < len && n + 1 < cap) {
            uint8_t c = img[at + n];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) break;
            out[n] = (char)c;
            n++;
        }
        // A name, ended by the space before "build=" (or the end of the string).
        if (n == 0 || at + n >= len || (img[at + n] != ' ' && img[at + n] != 0)) continue;
        out[n] = 0;
        return true;
    }
    return false;
}
