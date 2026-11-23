// image_fits_chip against the real release images of every board: each must pass
// on its own chip and be refused on the other. That refusal is the whole point;
// the check it replaced accepted both layouts on both chips, so an RP2040 image
// would have been written over an RP2350's firmware.
#include "imagecheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int checks = 0, fails = 0;
static void ck(bool ok, const char *what) {
    checks++;
    if (!ok) { fails++; printf("  FAIL: %s\n", what); }
}

static bool load(const char *board, std::vector<uint8_t> &out) {
    char path[160];
    snprintf(path, sizeof(path), "../../releases/rpcortex-v2-%s.bin", board);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return !out.empty();
}

int main(void) {
    struct { const char *board; ImageChip chip, other; } boards[] = {
        {"pico",    IMAGE_CHIP_RP2040, IMAGE_CHIP_RP2350},
        {"pico_w",  IMAGE_CHIP_RP2040, IMAGE_CHIP_RP2350},
        {"pico2",   IMAGE_CHIP_RP2350, IMAGE_CHIP_RP2040},
        {"pico2_w", IMAGE_CHIP_RP2350, IMAGE_CHIP_RP2040},
    };
    char what[128];
    for (auto &b : boards) {
        std::vector<uint8_t> img;
        // A missing image is a failure, never a skip: a skipped check counted
        // as a pass is how this kind of bug ships.
        snprintf(what, sizeof(what), "%s: the release image is readable", b.board);
        ck(load(b.board, img), what);
        if (img.empty()) continue;

        uint32_t sp = 0, entry = 0;
        snprintf(what, sizeof(what), "%s: accepted on its own chip", b.board);
        ck(image_fits_chip(img.data(), img.size(), b.chip, &sp, &entry), what);
        snprintf(what, sizeof(what), "%s: reports the stack it checked (%08x)", b.board, sp);
        ck(sp >= 0x20000000u, what);
        snprintf(what, sizeof(what), "%s: refused on the other chip", b.board);
        ck(!image_fits_chip(img.data(), img.size(), b.other, nullptr, nullptr), what);

        // Only the first 4 KB is examined on the device; that must be enough.
        snprintf(what, sizeof(what), "%s: decided by its first 4 KB", b.board);
        ck(image_fits_chip(img.data(), 4096, b.chip, nullptr, nullptr), what);

        // Shorter than its vector table is not an image.
        snprintf(what, sizeof(what), "%s: a truncated image is refused", b.board);
        ck(!image_fits_chip(img.data(), b.chip == IMAGE_CHIP_RP2040 ? 0x104 : 4, b.chip, nullptr, nullptr), what);

        if (b.chip == IMAGE_CHIP_RP2350) {
            // Remove the picobin block's marker: the boot ROM would not run it.
            std::vector<uint8_t> nb = img;
            for (size_t i = 0; i + 4 <= 4096; i += 4) {
                uint32_t w; memcpy(&w, &nb[i], 4);
                if (w == 0xffffded3u) memset(&nb[i], 0, 4);
            }
            snprintf(what, sizeof(what), "%s: refused without its picobin block", b.board);
            ck(!image_fits_chip(nb.data(), nb.size(), b.chip, nullptr, nullptr), what);
        }
    }

    // Things that are not firmware at all.
    std::vector<uint8_t> zeros(8192, 0), ff(8192, 0xff), text(8192, 'A');
    for (ImageChip c : {IMAGE_CHIP_RP2040, IMAGE_CHIP_RP2350}) {
        ck(!image_fits_chip(zeros.data(), zeros.size(), c, nullptr, nullptr), "zeros refused");
        ck(!image_fits_chip(ff.data(), ff.size(), c, nullptr, nullptr), "erased flash refused");
        ck(!image_fits_chip(text.data(), text.size(), c, nullptr, nullptr), "text refused");
        ck(!image_fits_chip(nullptr, 0, c, nullptr, nullptr), "nothing refused");
    }

    // An RP2350 table whose stack is past the end of an RP2040's SRAM, moved to
    // 0x100 to look like an RP2040 image: refused on the RP2040.
    std::vector<uint8_t> fake(4096, 0);
    uint32_t pair[2] = {0x20082000u, 0x1000015bu};
    memcpy(&fake[0x100], pair, 8);
    ck(!image_fits_chip(fake.data(), fake.size(), IMAGE_CHIP_RP2040, nullptr, nullptr),
       "an RP2040 layout with an RP2350 stack is refused");
    // An even entry point is not Thumb code.
    pair[0] = 0x20042000u; pair[1] = 0x100001f6u;
    memcpy(&fake[0x100], pair, 8);
    ck(!image_fits_chip(fake.data(), fake.size(), IMAGE_CHIP_RP2040, nullptr, nullptr),
       "an even entry point is refused");

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
