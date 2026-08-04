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
static uint32_t on_return_unused(void) { return 0x10006000u; }

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

    // --- a package's supervisor call ----------------------------------------
    //
    // A sandboxed package cannot branch into the firmware, so it names the
    // function it wants by index and executes SVC. The handler turns that into
    // a call by rewriting where the exception returns to. Every field it
    // touches is one that fails without saying anything: the wrong PC jumps
    // into nothing, a lost LR returns the package to whatever was in that word,
    // and a clobbered r0-r3 loses the arguments in a way that looks like the
    // firmware function misbehaving.
    {
        const uint32_t target    = 0x10005000u;   // a firmware function
        const uint32_t on_return = 0x10006000u;   // the way back
        uint32_t f[EXC_FRAME_WORDS];
        make_frame(f, 0x20001004u, XPSR_THUMB);
        f[EXC_LR_WORD] = 0x20002222u;             // where the package expects to resume
        f[EXC_R12_WORD] = 42;                     // the index it asked for
        uint32_t saved = 0;

        ok(exc_frame_syscall_index(f) == 42, "the index comes from the stacked r12");

        ok(exc_frame_enter_firmware(f, target, on_return, &saved) == SYSCALL_OK,
           "a call into the firmware is set up");
        ok(f[EXC_PC_WORD] == (target | 1u), "the return lands in the firmware function");
        ok(f[EXC_LR_WORD] == (on_return | 1u), "which will return to the way back");
        ok(saved == 0x20002222u,
           "and the package's own return address is handed back, not discarded");
        ok(f[EXC_XPSR_WORD] & XPSR_THUMB, "Thumb state is kept");

        // The arguments. r0-r3 are what the package put there and the firmware
        // function is about to read them.
        bool args_kept = true;
        for (int i = 0; i <= 3; i++) if (f[i] != 0xA0000000u + (uint32_t)i) args_kept = false;
        ok(args_kept, "r0-r3 are untouched, because they are the arguments");
    }
    {
        // An index the table does not have. The frame must be left EXACTLY as
        // it was: a half-rewritten frame returns somewhere nobody chose, and
        // the report would then describe that jump instead of the request.
        uint32_t f[EXC_FRAME_WORDS], before[EXC_FRAME_WORDS];
        make_frame(f, 0x20001004u, XPSR_THUMB);
        f[EXC_LR_WORD] = 0x20002222u;
        memcpy(before, f, sizeof(f));
        uint32_t saved = 0xDEADBEEFu;

        ok(exc_frame_enter_firmware(f, 0, on_return_unused(), &saved) == SYSCALL_BAD_INDEX,
           "an index the firmware does not export is refused");
        ok(memcmp(f, before, sizeof(f)) == 0, "and nothing at all is written");

        ok(exc_frame_enter_firmware(nullptr, 0x10005000u, 0x10006000u, &saved)
               == SYSCALL_NO_FRAME, "and there being no frame is its own answer");
    }

    // --- null safety --------------------------------------------------------
    exc_frame_redirect(nullptr, handler);
    ok(true, "a null frame does not crash");

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
