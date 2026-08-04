#include "excframe.h"

void exc_frame_redirect(uint32_t *frame, uint32_t handler) {
    if (!frame) return;
    frame[EXC_PC_WORD] = handler | 1u;
    frame[EXC_XPSR_WORD] = (frame[EXC_XPSR_WORD] & ~XPSR_IT_ICI) | XPSR_THUMB;
}

bool exc_return_used_psp(uint32_t exc_return) { return (exc_return & 4u) != 0; }

uint32_t exc_frame_syscall_index(const uint32_t *frame) {
    return frame ? frame[EXC_R12_WORD] : 0xFFFFFFFFu;
}

SyscallCheck exc_frame_enter_firmware(uint32_t *frame, uint32_t target,
                                      uint32_t on_return, uint32_t *saved_lr) {
    if (!frame) return SYSCALL_NO_FRAME;
    // Nothing is written on a refusal. A half-rewritten frame returns somewhere
    // that was never intended, and the report would then describe the jump
    // rather than the request that caused it.
    if (!target) return SYSCALL_BAD_INDEX;

    if (saved_lr) *saved_lr = frame[EXC_LR_WORD];

    // r0-r3 are left exactly as the package set them: they are the arguments,
    // and this is a call, not a new context.
    frame[EXC_PC_WORD] = target | 1u;
    frame[EXC_LR_WORD] = on_return | 1u;
    frame[EXC_XPSR_WORD] = (frame[EXC_XPSR_WORD] & ~XPSR_IT_ICI) | XPSR_THUMB;
    return SYSCALL_OK;
}
