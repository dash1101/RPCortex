// Detached shell runs: the table behind fw_shell_run_detached.
//
// Everything here decides who may read an answer and when a slot may be handed
// on. Getting it wrong is not a crash — it is one package collecting another's
// output, or a handle kept a moment too long landing on somebody else's run. So
// the failure cases below outnumber the working one on purpose.

#include "detach.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

// Stand-ins for two loaded packages. Only the ADDRESS matters — the real thing
// is a package's image pointer, which is exactly this: a token nothing else can
// forge because nothing else has it.
static char g_pkg_a[4];
static char g_pkg_b[4];
#define PKG_A ((const void *)g_pkg_a)
#define PKG_B ((const void *)g_pkg_b)

static char g_out_a[64];
static char g_out_b[64];

int main(void) {
    detach_reset();

    // --- the ordinary run ----------------------------------------------------
    int h = detach_claim(PKG_A, "wifi scan", g_out_a, sizeof(g_out_a));
    ck(h >= 0, "a package can claim a detached run");
    ck(detach_active() == 1, "and it holds a slot while it runs");

    DetachRun *r = detach_find(h, PKG_A);
    ck(r != nullptr, "the package that claimed it can find it");
    ck(r && strcmp(r->line, "wifi scan") == 0, "the line was copied in");
    ck(r && !r->done, "and it has not finished yet");

    // ANOTHER PACKAGE MAY NOT TOUCH IT. The handle is a small integer, so a
    // package guessing one is not a stretch — the owner is what makes guessing
    // pointless.
    ck(detach_find(h, PKG_B) == nullptr, "another package cannot find it");
    ck(detach_find(h, nullptr) == nullptr, "and neither can a caller with no identity");

    // The capture buffer belongs to this run, because this run asked for output.
    char *cap = detach_capture_buffer(h);
    ck(cap != nullptr, "a run that wants output gets the capture buffer");

    detach_finish(h, 3);
    ck(r->done == 1, "finishing marks it");
    ck(r->status == 3, "and records what the command returned");
    detach_finish(h, 99);
    ck(r->status == 3, "a second finish does not overwrite the first");

    detach_release(h);
    ck(detach_active() == 0, "collecting it gives the slot back");
    ck(detach_find(h, PKG_A) == nullptr, "and the handle stops working");

    // --- a stale handle must not land on the next run ------------------------
    //
    // The case this is really for: a screen fires a command per keypress, so a
    // slot is claimed and released over and over. Without a generation, a handle
    // held one frame too long reads the NEXT command's exit status as its own.
    int h1 = detach_claim(PKG_A, "df", nullptr, 0);
    detach_finish(h1, 0);
    detach_release(h1);
    int h2 = detach_claim(PKG_A, "free", nullptr, 0);
    ck(h2 != h1, "a reclaimed slot hands out a different handle");
    ck(detach_find(h1, PKG_A) == nullptr, "the old handle does not find the new run");
    ck(detach_find(h2, PKG_A) != nullptr, "and the new one does");
    detach_release(h2);

    // --- the slot cap --------------------------------------------------------
    detach_reset();
    int held[DETACH_MAX];
    for (int i = 0; i < DETACH_MAX; i++) {
        held[i] = detach_claim(PKG_A, "ps", nullptr, 0);
        ck(held[i] >= 0, "a slot is available up to the cap");
    }
    ck(detach_claim(PKG_A, "ps", nullptr, 0) < 0,
       "past the cap, with everything still running, it is refused");
    ck(detach_active() == DETACH_MAX, "and nothing was quietly started anyway");

    // A run that FINISHED and was never collected is fair game. Its answer has
    // been sitting unread; a package that fires runs and never polls must cost a
    // bounded amount rather than jamming the table for good.
    detach_finish(held[0], 7);
    int h3 = detach_claim(PKG_A, "df", nullptr, 0);
    ck(h3 >= 0, "an uncollected finished run is reclaimed to make room");
    ck(detach_find(held[0], PKG_A) == nullptr, "the reclaimed handle stops working");
    ck(detach_find(held[DETACH_MAX - 1], PKG_A) != nullptr,
       "and the one still running was left alone");

    // --- one output capture --------------------------------------------------
    //
    // There is a single output capture in the OS, so a second run wanting output
    // would come back with an empty buffer that reads exactly like a command
    // which printed nothing. Refused at the point of asking instead.
    detach_reset();
    int c1 = detach_claim(PKG_A, "ls", g_out_a, sizeof(g_out_a));
    ck(c1 >= 0, "the first run may capture");
    ck(detach_claim(PKG_A, "df", g_out_b, sizeof(g_out_b)) < 0,
       "a second run wanting output is refused while the first is capturing");
    int c2 = detach_claim(PKG_A, "df", nullptr, 0);
    ck(c2 >= 0, "but one that wants no output still starts");
    ck(detach_capture_buffer(c2) == nullptr, "and it is given no buffer");
    ck(detach_capture_buffer(c1) != nullptr, "the capturing run keeps it");

    detach_finish(c1, 0);
    detach_release(c1);
    int c3 = detach_claim(PKG_A, "free", g_out_b, sizeof(g_out_b));
    ck(c3 >= 0, "collecting the capturing run frees the capture for the next");
    ck(detach_capture_buffer(c3) != nullptr, "which then has it");

    // --- a package that unloads ----------------------------------------------
    //
    // The run is the firmware's and carries on; what goes is the package's claim
    // on it, so nothing can be collected into memory the heap has taken back.
    detach_reset();
    int u1 = detach_claim(PKG_A, "wifi scan", g_out_a, sizeof(g_out_a));
    int u2 = detach_claim(PKG_B, "ps", nullptr, 0);
    detach_forget_owner(PKG_A);
    ck(detach_find(u1, PKG_A) == nullptr, "an unloaded package cannot collect its run");
    ck(detach_of(u1) != nullptr, "but the run keeps its slot while the task is alive");
    {
        DetachRun *orphan = detach_of(u1);
        ck(orphan && orphan->pkg_out == nullptr,
           "and the pointer into its memory is dropped");
    }
    ck(detach_find(u2, PKG_B) != nullptr, "another package's run is untouched");

    // A run that had already finished is nobody's, so the slot goes back at once.
    detach_reset();
    int u3 = detach_claim(PKG_A, "df", nullptr, 0);
    detach_finish(u3, 0);
    detach_forget_owner(PKG_A);
    ck(detach_active() == 0, "a finished run is released when its package unloads");

    // --- what is refused up front --------------------------------------------
    detach_reset();
    ck(detach_claim(nullptr, "ls", nullptr, 0) < 0, "a caller with no identity is refused");
    ck(detach_claim(PKG_A, "", nullptr, 0) < 0, "an empty line is refused");
    ck(detach_claim(PKG_A, nullptr, nullptr, 0) < 0, "a null line is refused");

    // A LINE TOO LONG IS REFUSED, NOT TRUNCATED. Half a shell line is a
    // different command, and one that happens to still parse is the worst kind:
    // `rm -r /os/pkg/name` cut short is `rm -r /os/pkg`.
    char big[DETACH_LINE_MAX + 8];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    ck(detach_claim(PKG_A, big, nullptr, 0) < 0, "a line longer than the buffer is refused");
    ck(detach_active() == 0, "and no slot was spent on it");

    // Exactly at the limit still works — an off-by-one here would refuse a line
    // the shell itself would have accepted.
    char full[DETACH_LINE_MAX];
    memset(full, 'y', sizeof(full) - 1);
    full[sizeof(full) - 1] = 0;
    int hf = detach_claim(PKG_A, full, nullptr, 0);
    ck(hf >= 0, "a line that exactly fills the buffer is accepted");
    ck(detach_of(hf) && strlen(detach_of(hf)->line) == DETACH_LINE_MAX - 1,
       "and arrives whole");

    // Nothing to collect from a handle nobody ever issued.
    ck(detach_find(-1, PKG_A) == nullptr, "a negative handle finds nothing");
    ck(detach_find(9999, PKG_A) == nullptr, "nor does one out of range");

    printf("  detach: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
