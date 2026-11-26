// The A/B firmware layout on RP2350, as arithmetic (no hardware), so the host
// tests can check it. storage.cpp applies it.
//
// The reserve at the start of flash (RPC_FW_RESERVE) holds:
//
//   0              the partition table (slot 0, 4 KB)
//   4 KB           slot 1, blank (the boot ROM looks there too)
//   8 KB           partition A
//   8 KB + P       partition B           P = reserve / 2 - 4 KB
//
// and the boot ROM maps the partition it booted at 0x10000000 and NOTHING else:
// flash beyond the partition's size faults (RP2350 datasheet 5.1.19). The
// firmware remaps at start-up so the flash after the running partition (the
// package slot, the filesystem, and B when A is running) follows it in the
// cached window; flash before it (the table, and A when B is running) is read
// through the uncached, untranslated alias at 0x1c000000.
#ifndef RPC_ABLAYOUT_H
#define RPC_ABLAYOUT_H

#include <stdint.h>

#define AB_TABLE_BYTES   8192u           // slot 0 (the table) and slot 1
#define AB_XIP           0x10000000u     // cached, translated
#define AB_XIP_RAW       0x1c000000u     // uncached, untranslated

static inline uint32_t ab_part_bytes(uint32_t reserve) { return reserve / 2u - 4096u; }

static inline uint32_t ab_part_off(uint32_t reserve, int part) {
    return AB_TABLE_BYTES + (uint32_t)part * ab_part_bytes(reserve);
}

// Which partition is running, from where the boot ROM mapped 0x10000000:
// 0 (A), 1 (B), or -1 when there is no partition table and the image is at 0
// (a board updated onto this firmware by one from before A/B).
static inline int ab_which(uint32_t reserve, uint32_t mapped_base) {
    if (mapped_base == ab_part_off(reserve, 0)) return 0;
    if (mapped_base == ab_part_off(reserve, 1)) return 1;
    return -1;
}

// Where flash offset `off` can be read, when `base` is mapped at 0x10000000 for
// `mapped` bytes.
static inline uint32_t ab_xip_addr(uint32_t base, uint32_t mapped, uint32_t off) {
    if (off >= base && off - base < mapped) return AB_XIP + (off - base);
    return AB_XIP_RAW + off;
}

// The version an image's boot block declares (the SDK's crt0 emits a VERSION
// item: 0x48, 2 words, then minor and major as 16-bit values), found in its
// first `len` bytes. False when it declares none.
static inline bool ab_image_version(const uint8_t *img, uint32_t len,
                                    uint16_t *major, uint16_t *minor) {
    // The block marker, then the item, within the first block.
    for (uint32_t i = 0; i + 4 <= len && i < 4096u; i += 4) {
        uint32_t w = (uint32_t)img[i] | ((uint32_t)img[i + 1] << 8) |
                     ((uint32_t)img[i + 2] << 16) | ((uint32_t)img[i + 3] << 24);
        if (w != 0xffffded3u) continue;
        for (uint32_t j = i + 4; j + 8 <= len && j < i + 256u; j += 4) {
            if (img[j] == 0x48 && img[j + 1] == 0x02 && img[j + 2] == 0 && img[j + 3] == 0) {
                *minor = (uint16_t)(img[j + 4] | (img[j + 5] << 8));
                *major = (uint16_t)(img[j + 6] | (img[j + 7] << 8));
                return true;
            }
            if (img[j] == 0x79 && img[j + 1] == 0x35 && img[j + 2] == 0x12 && img[j + 3] == 0xab)
                break;                    // the block's end marker
        }
        return false;
    }
    return false;
}

#endif
