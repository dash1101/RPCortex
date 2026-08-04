#include "ptrcheck.h"

static uint32_t g_refusals;

uint32_t ptr_refusals(void) { return g_refusals; }
void     ptr_note_refusal(void) { g_refusals++; }

// One region, and whether the firmware may write through it.
struct Span { uintptr_t base; uint32_t size; bool writable; };

// Does [a, a+len) sit entirely inside [base, base+size)?
//
// Written so the arithmetic cannot wrap. `a + len` on a pointer near the top of
// the address space overflows, and an overflowed comparison says yes to exactly
// the pointer that should be refused — a length chosen to wrap is the first
// thing anyone would try.
static bool within(uintptr_t a, uint32_t len, uintptr_t base, uint32_t size) {
    if (!size) return false;
    if (a < base) return false;
    uintptr_t off = a - base;
    if (off > size) return false;
    return len <= size - off;      // never computes a + len
}

// The five regions, in the order a package is most likely to be pointing at.
static uint32_t spans_of(const TaskAppMem *m, Span *out) {
    uint32_t n = 0;
    // Its own heap and stack first: almost every buffer a package passes came
    // from one of the two.
    out[n++] = { (uintptr_t)m->arena,  m->arena_size,  true  };
    out[n++] = { (uintptr_t)m->stack,  m->stack_size,  true  };
    out[n++] = { (uintptr_t)m->data,   m->data_size,   true  };
    // Code and the trampolines are readable but never writable. A string
    // literal lives in text, so reads land here often; a WRITE here is either a
    // bug or an attempt to rewrite the package's own code, and neither should
    // be helped along by the firmware.
    out[n++] = { (uintptr_t)m->text,   m->text_size,   false };
    out[n++] = { (uintptr_t)m->veneer, m->veneer_size, false };
    return n;
}

bool ptr_ok(const TaskAppMem *mem, const void *p, uint32_t len, PtrAccess acc) {
    // Not a sandboxed package: the shell, a driver, an OS task. Those already
    // run privileged and could reach the memory without asking, so refusing
    // them would protect nothing and break everything.
    if (!mem) return true;
    if (!len) return true;              // no byte is touched
    if (!p) return false;

    Span s[5];
    uint32_t n = spans_of(mem, s);
    for (uint32_t i = 0; i < n; i++) {
        if (!within((uintptr_t)p, len, s[i].base, s[i].size)) continue;
        if (acc == PTR_WRITE && !s[i].writable) return false;
        return true;
    }
    return false;
}

bool ptr_str_ok(const TaskAppMem *mem, const char *s, uint32_t *len_out) {
    if (len_out) *len_out = 0;
    if (!mem) {
        // Not a package. The length is still wanted by some callers, and there
        // is nothing to bound it against, so it is not computed — a caller that
        // needs it for an unsandboxed pointer can use strlen itself.
        return s != nullptr;
    }
    if (!s) return false;

    Span sp[5];
    uint32_t n = spans_of(mem, sp);
    for (uint32_t i = 0; i < n; i++) {
        if (!sp[i].size) continue;
        uintptr_t a = (uintptr_t)s;
        if (a < sp[i].base) continue;
        uintptr_t off = a - sp[i].base;
        if (off >= sp[i].size) continue;

        // Scan only to the end of the region that holds the start. A string
        // that runs off the end is refused rather than followed into whatever
        // happens to be next, which is the whole point: the terminator is data
        // the package controls, so it cannot be trusted to arrive.
        uint32_t room = sp[i].size - (uint32_t)off;
        for (uint32_t k = 0; k < room; k++) {
            if (s[k] == 0) { if (len_out) *len_out = k; return true; }
        }
        return false;
    }
    return false;
}
