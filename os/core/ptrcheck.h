// Checking the pointers a package hands across the ABI.
//
// A sandboxed package cannot touch the OS's memory: the protection unit stops
// it. But it can ASK the OS to, and until now the OS did as it was told —
// `fw_file_read(path, buf, cap)` wrote `cap` bytes wherever `buf` pointed, and
// the write happened in privileged code where the protection unit does not
// apply. Every pointer argument was a way to read or write anything on the
// machine, from inside the sandbox, without breaking it.
//
// That is the difference between a sandbox that stops a package having a bug
// and one that stops a package being hostile. The stated worry has always been
// the first, but the second is what "cannot reach the rest of the system"
// implies, and it was not true.
//
// The check is a range test against the five regions the package was given —
// the same base and size figures the protection unit itself was programmed
// from, so the two cannot disagree about what a package owns. It is arithmetic
// rather than an instruction, which is deliberate: ARMv8-M has TT for exactly
// this, but TT needs the security extension enabled to be reached from C, its
// result encoding is easy to decode wrongly in a way that fails open, and
// ARMv6-M has no equivalent at all. One rule that works on both parts and can
// be tested on a host beats a hardware feature on one of them.
#ifndef RPC_PTRCHECK_H
#define RPC_PTRCHECK_H

#include <stdint.h>
#include "task.h"

enum PtrAccess {
    PTR_READ,     // the firmware will read these bytes
    PTR_WRITE,    // the firmware will write them
};

// Whether a package owning `mem` may have the firmware touch [p, p+len).
//
// A null `mem` means the caller is not a sandboxed package — the shell, a
// driver, an OS task — and everything is allowed. That is not a hole: those
// callers already run privileged and could reach the memory directly.
//
// A zero length is allowed whatever `p` is, because no byte is touched. Callers
// that dereference regardless are the bug, and the check cannot see that.
bool ptr_ok(const TaskAppMem *mem, const void *p, uint32_t len, PtrAccess acc);

// The same, for a NUL-terminated string the package supplied.
//
// Separate because the length is not known until the string has been read, and
// reading it is the thing that has to be bounded: a path with no terminator
// would otherwise walk out of the package's data and into whatever follows,
// which is the bug this exists to stop rather than an unlikely accident.
//
// Returns false when the string starts outside the package, or when no
// terminator appears before the end of the region holding it. `len_out` gets
// the length, not counting the terminator.
bool ptr_str_ok(const TaskAppMem *mem, const char *s, uint32_t *len_out);

// How many calls have been refused, for `mpu` to report.
//
// Worth watching rather than merely counting: a refusal is a package asking the
// firmware to touch something outside itself. One is a bug in that package. A
// stream of them is a package doing it on purpose.
uint32_t ptr_refusals(void);
void     ptr_note_refusal(void);

#endif
