// Checking the pointers a package hands across the ABI.
//
// This is the last hole in the sandbox, so the test is written for a hostile
// caller rather than a careless one. A careless package passes a pointer that
// is merely wrong; a hostile one passes the pointer that is wrong in the way
// the check does not look at — one byte past the end, a length that wraps the
// address space, a string with no terminator, a write aimed at its own code.
//
// Every one of those fails OPEN if the arithmetic is written the obvious way,
// which is why they are all here.
#include "../core/ptrcheck.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

// A package's five regions, laid out the way the loader lays them out.
static uint8_t text[256], veneer[64], data[128], stack[512], arena[1024];
static TaskAppMem mem;

// Somewhere the package does NOT own, standing in for the OS's own memory.
static uint8_t elsewhere[64];

int main(void) {
    printf("ptrcheck_test - the pointers a package hands across the ABI\n");

    memset(&mem, 0, sizeof(mem));
    mem.text = text;     mem.text_size = sizeof(text);
    mem.veneer = veneer; mem.veneer_size = sizeof(veneer);
    mem.data = data;     mem.data_size = sizeof(data);
    mem.stack = stack;   mem.stack_size = sizeof(stack);
    mem.arena = arena;   mem.arena_size = sizeof(arena);

    // --- what a package legitimately does -----------------------------------
    ck(ptr_ok(&mem, arena, 100, PTR_WRITE), "a buffer from its own heap can be written");
    ck(ptr_ok(&mem, stack + 8, 32, PTR_WRITE), "so can one on its own stack");
    ck(ptr_ok(&mem, data, sizeof(data), PTR_WRITE), "and a global");
    ck(ptr_ok(&mem, text, 16, PTR_READ), "a string literal in code can be read");

    // Exactly filling a region is legal. Off-by-one in the other direction
    // would refuse every full-buffer call a package makes.
    ck(ptr_ok(&mem, arena, sizeof(arena), PTR_WRITE), "a request for the whole arena fits");
    ck(ptr_ok(&mem, arena + sizeof(arena) - 1, 1, PTR_WRITE), "and so does its last byte");

    // --- what it must not be allowed to do ----------------------------------
    ck(!ptr_ok(&mem, elsewhere, 8, PTR_READ),
       "memory the package does not own cannot be read");
    ck(!ptr_ok(&mem, elsewhere, 8, PTR_WRITE),
       "and certainly not written");
    ck(!ptr_ok(&mem, text, 4, PTR_WRITE),
       "its own code is readable but not writable");
    ck(!ptr_ok(&mem, veneer, 4, PTR_WRITE),
       "and neither are the call trampolines");
    ck(!ptr_ok(&mem, nullptr, 4, PTR_READ), "null is refused");

    // One byte past the end. The classic, and it passes if the comparison is
    // written with <= instead of <.
    ck(!ptr_ok(&mem, arena, sizeof(arena) + 1, PTR_WRITE),
       "a length one byte past the end of a region is refused");
    ck(!ptr_ok(&mem, arena + sizeof(arena), 1, PTR_WRITE),
       "and so is a pointer one byte past it");
    ck(!ptr_ok(&mem, arena + sizeof(arena) - 4, 8, PTR_WRITE),
       "a buffer that starts inside and ends outside is refused as a whole");

    // A length chosen to wrap the address space. `p + len` overflows, and an
    // overflowed comparison says yes to exactly the pointer that should be
    // refused. This is the first thing anyone attacking it would try.
    ck(!ptr_ok(&mem, arena + 8, 0xFFFFFFFFu, PTR_WRITE),
       "a length that wraps the address space is refused");
    ck(!ptr_ok(&mem, arena, 0x80000000u, PTR_READ),
       "and so is one merely enormous");

    // Straddling two regions is refused even when both are owned: they are not
    // contiguous, and treating them as one would let a package read whatever
    // the allocator happened to put between them.
    {
        TaskAppMem m2 = mem;
        static uint8_t low[64], gap[64], high[64];
        (void)gap;
        m2.data = low;   m2.data_size = sizeof(low);
        m2.arena = high; m2.arena_size = sizeof(high);
        ck(!ptr_ok(&m2, low, sizeof(low) + 8, PTR_WRITE),
           "a read spanning out of one region into another is refused");
    }

    // Zero length touches nothing, so it is allowed wherever it points. A
    // caller that dereferences anyway is the bug, and no check can see that.
    ck(ptr_ok(&mem, elsewhere, 0, PTR_WRITE), "a zero-length access touches nothing");

    // --- not a package at all -----------------------------------------------
    //
    // The shell and the drivers already run privileged. Refusing them would
    // protect nothing and break everything.
    ck(ptr_ok(nullptr, elsewhere, 64, PTR_WRITE),
       "an unsandboxed caller is not restricted");

    // --- strings ------------------------------------------------------------
    {
        uint32_t len = 0;
        memcpy(data, "hello.txt", 10);
        ck(ptr_str_ok(&mem, (char *)data, &len) && len == 9,
           "a terminated string in the package is accepted, with its length");

        ck(!ptr_str_ok(&mem, (char *)elsewhere, &len),
           "a string outside the package is refused");
        ck(!ptr_str_ok(&mem, nullptr, &len), "and so is null");

        // No terminator before the end of the region. Following it would walk
        // into whatever the allocator put next, which is the bug this exists to
        // stop rather than an unlikely accident.
        memset(data, 'A', sizeof(data));
        ck(!ptr_str_ok(&mem, (char *)data, &len),
           "a string with no terminator inside its region is refused");

        // A terminator in the very last byte is still a valid string.
        data[sizeof(data) - 1] = 0;
        ck(ptr_str_ok(&mem, (char *)data, &len) && len == sizeof(data) - 1,
           "a string ending in the last byte of a region is accepted");

        // Starting at the last byte, with the terminator there, is the empty
        // string and is fine.
        data[sizeof(data) - 1] = 0;
        ck(ptr_str_ok(&mem, (char *)data + sizeof(data) - 1, &len) && len == 0,
           "an empty string at the very end is accepted");

        // One past the end of the LAST region, using a table with a single
        // region in it.
        //
        // Deliberately not "one past mem.data": the five arrays are separate
        // objects and the compiler may lay them out adjacently, so one past the
        // end of data can genuinely be the first byte of stack — which the
        // package owns, and which the check should therefore accept. Under a
        // sanitizer the redzones hide that and the assertion passes for the
        // wrong reason. A table with one region has no such ambiguity.
        TaskAppMem solo;
        memset(&solo, 0, sizeof(solo));
        solo.data = data; solo.data_size = sizeof(data);
        data[sizeof(data) - 1] = 0;
        ck(ptr_str_ok(&solo, (char *)data + sizeof(data) - 1, &len),
           "the last byte of the only region is still inside it");
        ck(!ptr_str_ok(&solo, (char *)data + sizeof(data), &len),
           "a string starting one byte past the only region is refused");
        ck(!ptr_ok(&solo, data + sizeof(data), 1, PTR_WRITE),
           "and so is a write there");

        // A string in read-only code is fine to READ, which is what a string
        // argument is for — most package paths are literals.
        memcpy(text, "/os/pkg", 8);
        ck(ptr_str_ok(&mem, (char *)text, &len) && len == 7,
           "a literal in the package's code is a valid string argument");
    }

    // --- an empty or partial region table -----------------------------------
    //
    // A package on ARMv6-M runs privileged and has no stack or arena region.
    // The table it gets has zeroes in those slots, and a zero-sized region must
    // match nothing rather than matching everything from address zero.
    {
        TaskAppMem m3;
        memset(&m3, 0, sizeof(m3));
        m3.data = data; m3.data_size = sizeof(data);
        ck(!ptr_ok(&m3, nullptr, 4, PTR_READ), "a zero-sized region matches nothing");
        ck(!ptr_ok(&m3, (void *)4, 4, PTR_READ), "including addresses just above zero");
        ck(ptr_ok(&m3, data, 8, PTR_WRITE), "while the regions it does have still work");
    }

    // --- the counter --------------------------------------------------------
    {
        uint32_t before = ptr_refusals();
        ptr_note_refusal();
        ck(ptr_refusals() == before + 1, "refusals are counted, so mpu can report them");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
