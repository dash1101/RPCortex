// Is this firmware image built for this chip?
//
// The last check before an update erases the running firmware. An RP2040 image
// and an RP2350 image are both Cortex-M images, and each looks plausible to a
// check that only asks "is there a stack pointer and an entry point somewhere
// near the start". Written to the wrong chip, either one leaves a board that
// only BOOTSEL can bring back, so the check has to know which layout belongs to
// which chip:
//
//   RP2040  256-byte second-stage bootloader, then the vector table at 0x100.
//           264 KB of SRAM, so the initial stack is at most 0x20042000.
//   RP2350  the vector table at 0, and a picobin block (it starts with the word
//           0xffffded3) in the first 4 KB, which the boot ROM needs to run it.
//           520 KB of SRAM, so the initial stack is at most 0x20082000.
//
// Pure, with no hardware headers, so it is tested on the host against the real
// release images of every board.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum ImageChip { IMAGE_CHIP_RP2040, IMAGE_CHIP_RP2350 };

// True when `img` (the first `len` bytes of an image) is firmware for `chip`.
// `sp` and `entry`, when given, receive the pair that was examined, for the
// error message.
bool image_fits_chip(const uint8_t *img, size_t len, ImageChip chip,
                     uint32_t *sp, uint32_t *entry);
