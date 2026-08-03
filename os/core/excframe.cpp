#include "excframe.h"

void exc_frame_redirect(uint32_t *frame, uint32_t handler) {
    if (!frame) return;
    frame[EXC_PC_WORD] = handler | 1u;
    frame[EXC_XPSR_WORD] = (frame[EXC_XPSR_WORD] & ~XPSR_IT_ICI) | XPSR_THUMB;
}

bool exc_return_used_psp(uint32_t exc_return) { return (exc_return & 4u) != 0; }
