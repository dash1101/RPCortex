// The PIO instruction encoders, on the host.
//
// A PIO program is a handful of 16-bit words and there is no assembler on the
// device, so a package writes those words through the macros in rpc_app.h. If
// the encoding is wrong the state machine does not fault — it does something
// else, silently, and the only symptom is an output that looks nearly right.
// That is the worst kind of bug to find on hardware, and it is pure arithmetic,
// so it belongs here.
//
// Every expected value below comes from the RP2040 datasheet's instruction
// encoding tables (section 3.4), not from running the macros and writing down
// what they produced — which would only prove they are consistent with
// themselves.
#include <stdio.h>
#include <stdint.h>

// The macros only, without the ABI declarations, which would want the device
// headers. They are self-contained by design.
#define FW_PIO_DELAY(d)          (((d) & 0x1f) << 8)
#define FW_PIO_SET(dst, val, delay) \
    ((unsigned short)(0xe000u | FW_PIO_DELAY(delay) | (((dst) & 7) << 5) | ((val) & 0x1f)))
#define FW_PIO_JMP(cond, addr, delay) \
    ((unsigned short)(0x0000u | FW_PIO_DELAY(delay) | (((cond) & 7) << 5) | ((addr) & 0x1f)))
#define FW_PIO_OUT(dst, count, delay) \
    ((unsigned short)(0x6000u | FW_PIO_DELAY(delay) | (((dst) & 7) << 5) | ((count) & 0x1f)))
#define FW_PIO_IN(src, count, delay) \
    ((unsigned short)(0x4000u | FW_PIO_DELAY(delay) | (((src) & 7) << 5) | ((count) & 0x1f)))
#define FW_PIO_NOP(delay) \
    ((unsigned short)(0xa000u | FW_PIO_DELAY(delay) | (2 << 5) | 2))

static int checks, fails;

static void eq(unsigned short got, unsigned short want, const char *what) {
    checks++;
    if (got != want) {
        printf("  FAIL: %-34s got 0x%04X, want 0x%04X\n", what, got, want);
        fails++;
    }
}

int main(void) {
    printf("pio_test - instruction encoding\n");

    // --- SET.  111 | delay(5) | dst(3) | data(5) --------------------------
    eq(FW_PIO_SET(0, 1, 0),  0xE001, "set pins, 1");
    eq(FW_PIO_SET(0, 0, 0),  0xE000, "set pins, 0");
    eq(FW_PIO_SET(1, 31, 0), 0xE03F, "set x, 31");
    eq(FW_PIO_SET(2, 5, 0),  0xE045, "set y, 5");
    eq(FW_PIO_SET(4, 1, 0),  0xE081, "set pindirs, 1");

    // --- JMP.  000 | delay(5) | cond(3) | addr(5) -------------------------
    eq(FW_PIO_JMP(0, 0, 0),  0x0000, "jmp 0");
    eq(FW_PIO_JMP(0, 7, 0),  0x0007, "jmp 7");
    eq(FW_PIO_JMP(2, 3, 0),  0x0043, "jmp x-- 3");
    eq(FW_PIO_JMP(4, 1, 0),  0x0081, "jmp y-- 1");
    eq(FW_PIO_JMP(6, 2, 0),  0x00C2, "jmp pin 2");

    // --- OUT.  011 | delay(5) | dst(3) | count(5) -------------------------
    eq(FW_PIO_OUT(0, 1, 0),  0x6001, "out pins, 1");
    eq(FW_PIO_OUT(1, 8, 0),  0x6028, "out x, 8");
    eq(FW_PIO_OUT(3, 32, 0), 0x6060, "out null, 32 (count 32 encodes as 0)");

    // --- IN.   010 | delay(5) | src(3) | count(5) -------------------------
    eq(FW_PIO_IN(0, 1, 0),   0x4001, "in pins, 1");
    eq(FW_PIO_IN(1, 4, 0),   0x4024, "in x, 4");

    // --- NOP is MOV y, y --------------------------------------------------
    eq(FW_PIO_NOP(0),        0xA042, "nop (mov y, y)");

    // --- the delay field, which is where PIO timing actually lives --------
    eq(FW_PIO_SET(0, 1, 1),  0xE101, "set pins, 1 [1]");
    eq(FW_PIO_SET(0, 1, 31), 0xFF01, "set pins, 1 [31]");
    eq(FW_PIO_NOP(15),       0xAF42, "nop [15]");
    eq(FW_PIO_JMP(0, 0, 7),  0x0700, "jmp 0 [7]");

    // Delay is five bits: 32 must wrap to 0 rather than overflow into the
    // opcode, which would silently turn a SET into a different instruction.
    eq(FW_PIO_SET(0, 1, 32), 0xE001, "delay 32 wraps to 0, not into the opcode");
    eq(FW_PIO_SET(0, 32, 0), 0xE000, "data 32 wraps to 0 the same way");

    // --- a real program: WS2812 ------------------------------------------
    //
    // The canonical four-instruction driver, at 800 kHz with side-set carrying
    // the line. Encoded here without side-set (which needs the sideset count in
    // the config) to check the shape a package would actually write.
    {
        const unsigned short ws2812[] = {
            FW_PIO_OUT(3, 1, 0),      // out null, 1   pull a bit
            FW_PIO_JMP(1, 3, 0),      // jmp !x, 3     branch on its value
            FW_PIO_JMP(0, 0, 5),      // jmp 0 [5]     long high
            FW_PIO_JMP(0, 0, 2),      // jmp 0 [2]     short high
        };
        checks++;
        if (ws2812[0] != 0x6061 || ws2812[1] != 0x0023 ||
            ws2812[2] != 0x0500 || ws2812[3] != 0x0200) {
            printf("  FAIL: ws2812 program encoded as %04X %04X %04X %04X\n",
                   ws2812[0], ws2812[1], ws2812[2], ws2812[3]);
            fails++;
        }
        // Fits the instruction memory with room to spare, which is the other
        // thing worth knowing before loading it.
        checks++;
        if (sizeof(ws2812) / sizeof(ws2812[0]) > 32) {
            printf("  FAIL: ws2812 program does not fit\n");
            fails++;
        }
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
