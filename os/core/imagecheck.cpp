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
