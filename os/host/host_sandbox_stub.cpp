// The sandbox seam on the host.
//
// There is no protection hardware here and no unprivileged mode, so
// sandbox_supported is false and everything that follows from it never runs.
// What the host DOES exercise is the path a board without the hardware takes —
// which is the same path an RP2040 takes, so it is worth having compiled.
#include "sandbox.h"

bool sandbox_supported(void) { return false; }
int  sandbox_enter(void *, int, void *, void *, uint32_t, uint32_t, uint32_t, uint32_t) { return -1; }
bool sandbox_in_package(void) { return false; }
void sandbox_counts(uint32_t *c, uint32_t *r) { if (c) *c = 0; if (r) *r = 0; }
void sandbox_release_for_kill(void) {}
void sandbox_forget(int) {}

// No protection unit on the host to dump.
extern "C" void mpu_dump_live(unsigned) {}
