// The exception-frame rewrite, checked without a Cortex-M.
//
// This is the mechanism behind terminating a task that will not stop yielding.
// Both fields it touches fail SILENTLY when wrong: a cleared Thumb bit faults
// somewhere that looks unrelated, and stale IT state makes the first
// instructions of the replacement function get skipped with no error at all.
// Neither would be found by looking at a running board.
#include "excframe.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

// A frame as the hardware would have left it: r0-r3, r12, LR, PC, xPSR.
static void make_frame(uint32_t *f, uint32_t pc, uint32_t xpsr) {
    for (int i = 0; i < EXC_FRAME_WORDS; i++) f[i] = 0xA0000000u + i;
    f[EXC_PC_WORD] = pc;
    f[EXC_XPSR_WORD] = xpsr;
}

int main(void) {
    printf("  excframe\n");

    const uint32_t handler = 0x10004321u;      // even, as a function address is

    // --- the redirect -------------------------------------------------------
    {
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0x20001000u, XPSR_THUMB);
        exc_frame_redirect(f, handler);

        ok(f[EXC_PC_WORD] == (handler | 1u), "PC points at the handler");
        ok(f[EXC_PC_WORD] & 1u, "PC carries the Thumb bit");
        ok(f[EXC_XPSR_WORD] & XPSR_THUMB, "xPSR keeps Thumb state");

        // Everything else must be untouched: r0-r3 and r12 are the task's, and
        // corrupting them would show up as nonsense inside the handler.
        bool others = true;
        for (int i = 0; i < EXC_FRAME_WORDS; i++) {
            if (i == EXC_PC_WORD || i == EXC_XPSR_WORD) continue;
            if (f[i] != 0xA0000000u + (uint32_t)i) others = false;
        }
        ok(others, "r0-r3, r12 and LR are left alone");
    }

    // --- Thumb state --------------------------------------------------------
    {
        // The pathological case: xPSR arrives with Thumb CLEAR. Returning like
        // that faults immediately, so it must be forced on rather than kept.
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0x20001000u, 0);
        exc_frame_redirect(f, handler);
        ok(f[EXC_XPSR_WORD] & XPSR_THUMB, "Thumb state is set even if it arrived clear");
    }
    {
        // An already-odd handler address must not be corrupted by setting the
        // bit twice — the loader bug in this tree was exactly a doubled Thumb
        // bit, and it produced a pointer two bytes past the function.
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0, XPSR_THUMB);
        exc_frame_redirect(f, handler | 1u);
        ok(f[EXC_PC_WORD] == (handler | 1u), "an already-odd handler is unchanged, not shifted");
    }

    // --- IT / ICI state -----------------------------------------------------
    {
        // An interrupt inside an IT block. Every condition bit set, plus Thumb.
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0x20001000u, XPSR_THUMB | XPSR_IT_ICI);
        exc_frame_redirect(f, handler);
        ok((f[EXC_XPSR_WORD] & XPSR_IT_ICI) == 0, "IT/ICI state is cleared");
        ok(f[EXC_XPSR_WORD] & XPSR_THUMB, "clearing IT does not take Thumb with it");
    }
    {
        // The two IT fields are split across the word (bits 26:25 and 15:10),
        // and clearing only one of them is the easy mistake.
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0, XPSR_THUMB | (3u << 25));
        exc_frame_redirect(f, handler);
        ok((f[EXC_XPSR_WORD] & (3u << 25)) == 0, "the high IT bits clear");

        make_frame(f, 0, XPSR_THUMB | (0x3fu << 10));
        exc_frame_redirect(f, handler);
        ok((f[EXC_XPSR_WORD] & (0x3fu << 10)) == 0, "the low IT bits clear");
    }
    {
        // Exception number and flags outside the fields being touched must
        // survive, or the return goes somewhere unrelated.
        uint32_t f[EXC_FRAME_WORDS];
        uint32_t keep = XPSR_THUMB | 0xF0000000u | 0x2Fu;   // NZCVQ and an exception number
        make_frame(f, 0, keep | XPSR_IT_ICI);
        exc_frame_redirect(f, handler);
        ok((f[EXC_XPSR_WORD] & 0xF0000000u) == 0xF0000000u, "condition flags survive");
        ok((f[EXC_XPSR_WORD] & 0x1FFu) == 0x2Fu, "the exception number survives");
    }

    // --- which stack --------------------------------------------------------
    {
        // 0xFFFFFFF9 returns to thread mode on the MAIN stack; 0xFFFFFFFD on
        // the PROCESS stack. Rewriting the wrong one corrupts an unrelated
        // frame, so this is checked rather than assumed.
        ok(!exc_return_used_psp(0xFFFFFFF9u), "EXC_RETURN 0xFFFFFFF9 is the main stack");
        ok(exc_return_used_psp(0xFFFFFFFDu),  "EXC_RETURN 0xFFFFFFFD is the process stack");
        ok(!exc_return_used_psp(0xFFFFFFF1u), "EXC_RETURN 0xFFFFFFF1 is handler mode, main stack");
    }

    // --- null safety --------------------------------------------------------
    exc_frame_redirect(nullptr, handler);
    ok(true, "a null frame does not crash");

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
