// A/B firmware layout on RP2350 (core/ablayout.h): where the partitions are,
// which one is running, where an offset is read from, and the version an image
// declares. Pure arithmetic; the board tests are the rest.
#include "../core/ablayout.h"

#include <stdio.h>
#include <string.h>
#include <vector>

static int g_fail, g_checks;
#define CK(c, what) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s (line %d)\n", what, __LINE__); } } while (0)

int main(int argc, char **argv) {
    // Pico 2 W: a 2 MB reserve.
    const uint32_t R = 2u * 1024 * 1024, FLASH = 4u * 1024 * 1024;
    CK(ab_part_bytes(R) == 1020u * 1024, "a partition is half the reserve less 4 KB");
    CK(ab_part_off(R, 0) == 0x2000, "A after the table and slot 1");
    CK(ab_part_off(R, 1) == 0x101000, "B right after A");
    CK(ab_part_off(R, 1) + ab_part_bytes(R) == R, "B ends where the reserve does: nothing after it moves");
    // Pico 2: a 1 MB reserve.
    CK(ab_part_off(1u << 20, 1) + ab_part_bytes(1u << 20) == (1u << 20), "the same on a 1 MB reserve");

    CK(ab_which(R, 0x2000) == 0, "mapped at A's start: A");
    CK(ab_which(R, 0x101000) == 1, "mapped at B's start: B");
    CK(ab_which(R, 0) == -1, "mapped at 0: no partitions (the old layout)");

    // Running from A: everything after A is in the cached window.
    uint32_t base = ab_part_off(R, 0), mapped = FLASH - base;
    CK(ab_xip_addr(base, mapped, 0x2000) == 0x10000000u, "A's start is 0x10000000");
    CK(ab_xip_addr(base, mapped, 0x200000) == 0x10000000u + 0x200000 - 0x2000, "the package slot follows");
    CK(ab_xip_addr(base, mapped, FLASH - 1) == 0x10000000u + FLASH - 1 - 0x2000, "the last byte of flash");
    CK(ab_xip_addr(base, mapped, 0) == 0x1c000000u, "the table: the untranslated alias");
    // Running from B: A and the table are before it.
    base = ab_part_off(R, 1); mapped = FLASH - base;
    CK(ab_xip_addr(base, mapped, 0x101000) == 0x10000000u, "B's start is 0x10000000");
    CK(ab_xip_addr(base, mapped, 0x2000) == 0x1c002000u, "A, from B: the untranslated alias");
    CK(ab_xip_addr(base, mapped, 0x300000) == 0x10000000u + 0x300000 - 0x101000, "the filesystem follows B");
    // No partitions: the identity.
    CK(ab_xip_addr(0, FLASH, 0x300000) == 0x10300000u, "the old layout reads as it always did");

    // The version an image declares: a block marker, the VERSION item, the end.
    std::vector<uint8_t> img(4096, 0xee);
    const uint8_t blk[] = {0xd3, 0xde, 0xff, 0xff,  0x42, 0x01, 0x21, 0x10,
                           0x48, 0x02, 0x00, 0x00,  0xb7, 0x02, 0x02, 0x00,
                           0xff, 0x00, 0x00, 0x00,  0x79, 0x35, 0x12, 0xab};
    memcpy(&img[0x138], blk, sizeof(blk));
    uint16_t maj = 0, min = 0;
    CK(ab_image_version(img.data(), (uint32_t)img.size(), &maj, &min) && maj == 2 && min == 695,
       "version 2.695 read from the block");
    std::vector<uint8_t> none(4096, 0xee);
    memcpy(&none[0x100], blk, 4);
    memcpy(&none[0x104], blk + 20, 4);                 // a block with no VERSION
    CK(!ab_image_version(none.data(), (uint32_t)none.size(), &maj, &min), "no VERSION: none");
    std::vector<uint8_t> blank(4096, 0xff);
    CK(!ab_image_version(blank.data(), (uint32_t)blank.size(), &maj, &min), "an erased sector: none");

    // A real image, when one has been built.
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (f) {
            std::vector<uint8_t> b(4096);
            size_t n = fread(b.data(), 1, b.size(), f);
            fclose(f);
            CK(ab_image_version(b.data(), (uint32_t)n, &maj, &min) && maj == 2 && min > 600,
               "the built image declares 2.<build>");
        }
    }
    printf("ablayout_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
