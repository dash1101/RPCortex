// The Nova D1 text helpers, on the host.
//
// These are the functions the settings commands build lists out of, and they got
// their second caller the day two problems appeared.
//
// One was REAL and would have shipped: the home-screen list was stored as the
// apps that are SHOWN, so hiding one meant first writing all thirty-two keys —
// about three hundred and fifty characters — into a registry value capped at
// ninety-six. Every app past the cutoff would have vanished from the home screen
// with the setting looking perfectly reasonable. It stores what is HIDDEN now,
// which is a handful of keys and has the better default besides.
//
// The other was LATENT: csv_remove wrote back using a hard-coded capacity rather
// than its caller's. No call site could actually reach past the end, because
// each of them fills its buffer with copy() first and copy truncates — but the
// next one to arrive with a smaller buffer would have. It takes a cap now, and
// the canary below pins that it stays inside it.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// The one struct the stubs need by name, taken from the ABI header rather than
// guessed at — a mismatched definition here would compile and then disagree.
#include "../include/rpc_app.h"

// The firmware side, stubbed. novacore is the leaf and touches very little of
// it; the text helpers below touch none at all.
extern "C" {
static char g_reg[16][2][128];
static int  g_regn;
int  fw_reg_get(const char *k, char *out, uint32_t cap) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(out, cap, "%s", g_reg[i][1]); return 1; }
    if (cap) out[0] = 0;
    return 0;
}
int fw_reg_set(const char *k, const char *v) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(g_reg[i][1], 128, "%s", v); return 1; }
    if (g_regn >= 16) return 0;
    snprintf(g_reg[g_regn][0], 128, "%s", k);
    snprintf(g_reg[g_regn][1], 128, "%s", v);
    g_regn++;
    return 1;
}
int32_t fw_reg_get_int(const char *k, int32_t d) {
    char b[128];
    if (!fw_reg_get(k, b, sizeof(b)) || !b[0]) return d;
    return (int32_t)atoi(b);
}
int  fw_reg_has(const char *k) { char b[4]; return fw_reg_get(k, b, sizeof(b)); }
void fw_reg_save(void) {}
int  fw_file_exists(const char *) { return 1; }
int  fw_mkdir(const char *) { return 1; }
// Always "the clock was never set", which is the case worth pinning: everything
// that formats a time has to render something honest rather than 00:00.
int  fw_time_get(struct FwTime *out) { (void)out; return 0; }
}

#include "../apps/novad1/novacore.cpp"

static int checks, failures;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { failures++; printf("    FAIL %s\n", what); }
}
static void seq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want)) { failures++; printf("    FAIL %s: got '%s' want '%s'\n", what, got, want); }
}

static void test_copy(void) {
    char b[8];
    ok(nova::copy(b, sizeof(b), "abc") == 3, "copy returns what it wrote");
    seq(b, "abc", "and writes it");
    nova::copy(b, sizeof(b), "abcdefghijkl");
    seq(b, "abcdefg", "a long string is truncated, not overrun");
    ok(b[7] == 0, "and still terminated");
    nova::copy(b, sizeof(b), nullptr);
    seq(b, "", "null is the empty string, not a crash");
}

static void test_csv_has(void) {
    ok(nova::csv_has("a,b,c", "b"), "a middle field is found");
    ok(nova::csv_has("a,b,c", "a"), "the first");
    ok(nova::csv_has("a,b,c", "c"), "and the last");
    ok(!nova::csv_has("a,b,c", "d"), "a missing one is not");
    // The failure that matters: a prefix must not match a longer field, or
    // hiding "res" would hide "resources" as well.
    ok(!nova::csv_has("resources", "res"), "a prefix is not a field");
    ok(!nova::csv_has("a,bb,c", "b"), "nor is a substring");
    ok(nova::csv_has("a, b ,c", "b"), "spaces around a field do not hide it");
    ok(!nova::csv_has("", "a"), "nothing contains nothing");
}

static void test_csv_add(void) {
    char b[32];
    b[0] = 0;
    ok(nova::csv_add(b, sizeof(b), "one"), "adding to an empty list works");
    seq(b, "one", "with no leading comma");
    ok(nova::csv_add(b, sizeof(b), "two"), "and again");
    seq(b, "one,two", "with one between them");
    ok(!nova::csv_add(b, sizeof(b), "one"), "a duplicate is refused");
    seq(b, "one,two", "and changes nothing");

    // The overflow case. It must refuse rather than write past the end.
    char small[10];
    nova::copy(small, sizeof(small), "abcdef");
    ok(!nova::csv_add(small, sizeof(small), "ghijkl"), "no room means no");
    seq(small, "abcdef", "and the list is untouched");
}

static void test_csv_remove(void) {
    char b[32];

    nova::copy(b, sizeof(b), "one,two,three");
    ok(nova::csv_remove(b, sizeof(b), "two"), "removing the middle works");
    seq(b, "one,three", "and rejoins the ends");

    nova::copy(b, sizeof(b), "one,two,three");
    ok(nova::csv_remove(b, sizeof(b), "one"), "the first");
    seq(b, "two,three", "leaves no leading comma");

    nova::copy(b, sizeof(b), "one,two,three");
    ok(nova::csv_remove(b, sizeof(b), "three"), "the last");
    seq(b, "one,two", "leaves no trailing comma");

    nova::copy(b, sizeof(b), "only");
    ok(nova::csv_remove(b, sizeof(b), "only"), "the only one");
    seq(b, "", "leaves an empty string");

    nova::copy(b, sizeof(b), "one,two");
    ok(!nova::csv_remove(b, sizeof(b), "three"), "removing what is not there is no change");
    seq(b, "one,two", "and leaves it alone");

    // A canary immediately past the buffer. This does not reproduce the old
    // code's failure — that needed an input already longer than the caller's
    // buffer, which copy() prevents — it pins the property the signature now
    // guarantees, so a future change that writes past `cap` is caught here.
    struct { char list[16]; char canary[8]; } guard;
    memset(&guard, 0, sizeof(guard));
    memcpy(guard.canary, "GUARDED", 8);
    nova::copy(guard.list, sizeof(guard.list), "aa,bb,cc,dd,ee");
    nova::csv_remove(guard.list, sizeof(guard.list), "cc");
    seq(guard.canary, "GUARDED", "csv_remove stays inside the buffer it was given");
    seq(guard.list, "aa,bb,dd,ee", "and still produces the right list");
}

static void test_ellipsize(void) {
    char b[32];
    nova::ellipsize(b, sizeof(b), "short", 10);
    seq(b, "short", "something that fits is left alone");
    nova::ellipsize(b, sizeof(b), "a much longer name", 8);
    ok(strlen(b) == 8, "something that does not is cut to width");
    seq(b + 6, "..", "and marked as cut");
}

static void test_reg_bool(void) {
    // Six spellings of the same thing, all of which a registry edited by three
    // different screens over six years actually contains.
    static const char *kTrue[]  = { "on", "true", "yes", "1", "ON", "True" };
    static const char *kFalse[] = { "off", "false", "no", "0", "OFF", "False" };
    for (unsigned i = 0; i < sizeof(kTrue) / sizeof(kTrue[0]); i++) {
        fw_reg_set("T", kTrue[i]);
        ok(nova::reg_bool("T", false), kTrue[i]);
        fw_reg_set("F", kFalse[i]);
        ok(!nova::reg_bool("F", true), kFalse[i]);
    }
    ok(nova::reg_bool("NeverSet", true), "an unset key gives the default");
    fw_reg_set("Junk", "banana");
    ok(nova::reg_bool("Junk", true), "and so does a value that means neither");
}

int main(void) {
    test_copy();
    test_csv_has();
    test_csv_add();
    test_csv_remove();
    test_ellipsize();
    test_reg_bool();
    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
