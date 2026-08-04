// Pulling a name out of a Bluetooth advertisement.
//
// The packet comes from whatever is nearby — something this device has no
// control over and no reason to trust. A length byte that runs past the end is
// the obvious way to make a scanner read memory that is not its own, and it is
// not hypothetical: anyone with a radio can send one.
//
// So the malformed cases below matter more than the well-formed ones.
#include "../core/btname.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %-46s got '%s', want '%s'\n", what, got, want);
        fails++;
    }
}

int main(void) {
    printf("btname_test - names out of advertisement data\n");
    char n[BT_NAME_MAX];

    // --- the ordinary shapes ------------------------------------------------
    {
        // len, type, "Pixel"
        const uint8_t ad[] = { 6, BT_AD_NAME_FULL, 'P','i','x','e','l' };
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 5, "a complete name is found");
        eq(n, "Pixel", "and read correctly");
    }
    {
        const uint8_t ad[] = { 4, BT_AD_NAME_SHORT, 'A','B','C' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "ABC", "a shortened name is used when it is all there is");
    }
    {
        // Flags first, then the name — the usual real layout.
        const uint8_t ad[] = { 2, 0x01, 0x06,
                               5, BT_AD_NAME_FULL, 'N','o','v','a' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "Nova", "a name after other records is still found");
    }
    {
        // Both present, short one FIRST. The complete name must still win, or
        // the answer depends on the advertiser's ordering.
        const uint8_t ad[] = { 3, BT_AD_NAME_SHORT, 'N','o',
                               5, BT_AD_NAME_FULL, 'N','o','v','a' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "Nova", "the complete name wins even when the short one came first");
    }
    {
        // And the other order, which a naive "last one wins" would get wrong.
        const uint8_t ad[] = { 5, BT_AD_NAME_FULL, 'N','o','v','a',
                               3, BT_AD_NAME_SHORT, 'N','o' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "Nova", "and when it came second");
    }

    // --- nothing to find ----------------------------------------------------
    {
        const uint8_t ad[] = { 2, 0x01, 0x06 };
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 0, "no name record gives nothing");
        eq(n, "", "and an empty, terminated string");
    }
    ck(bt_name_from_ad(nullptr, 0, n, sizeof(n)) == 0, "no data at all is handled");
    eq(n, "", "and still terminates the buffer");
    {
        const uint8_t ad[] = { 1, BT_AD_NAME_FULL };     // a record with no value
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 0, "an empty name is no name");
    }

    // --- malformed, which is the point of this file --------------------------
    {
        // A length claiming far more than the packet holds. Reading it would
        // walk off the end of whatever buffer the radio filled.
        const uint8_t ad[] = { 200, BT_AD_NAME_FULL, 'A','B' };
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 0,
           "a length running past the end of the packet is refused");
    }
    {
        // One byte too long: the off-by-one, which is the version that gets
        // written by accident rather than by an attacker.
        const uint8_t ad[] = { 4, BT_AD_NAME_FULL, 'A','B' };
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 0,
           "and so is one that overruns by a single byte");
    }
    {
        // Padding zeroes after a valid record. A zero length ends the list;
        // treating it as a record instead is an infinite loop.
        const uint8_t ad[] = { 5, BT_AD_NAME_FULL, 'N','o','v','a', 0, 0, 0, 0 };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "Nova", "trailing zero padding ends the list rather than looping");
    }
    {
        // A valid record after a broken one is unreachable, and must not be
        // reached by guessing where the next record starts.
        const uint8_t ad[] = { 90, 0x01, 0x06,
                               5, BT_AD_NAME_FULL, 'N','o','v','a' };
        ck(bt_name_from_ad(ad, sizeof(ad), n, sizeof(n)) == 0,
           "parsing stops at the first malformed record");
    }

    // --- hostile content -----------------------------------------------------
    {
        // A name is a string a stranger chose, and it ends up on a terminal.
        // Escape sequences in one would let a passer-by move the cursor or
        // recolour the screen of anyone who scans.
        const uint8_t ad[] = { 8, BT_AD_NAME_FULL, 'A', 0x1b, '[', '3', '1', 'm', 'B' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "A.[31mB", "control bytes in a name are replaced, not passed through");
    }
    {
        const uint8_t ad[] = { 4, BT_AD_NAME_FULL, '\n', '\r', '\t' };
        bt_name_from_ad(ad, sizeof(ad), n, sizeof(n));
        eq(n, "...", "including newlines, which would forge output lines");
    }

    // --- the caller's buffer -------------------------------------------------
    {
        const uint8_t ad[] = { 11, BT_AD_NAME_FULL, 'A','B','C','D','E','F','G','H','I','J' };
        char small[5];
        uint32_t got = bt_name_from_ad(ad, sizeof(ad), small, sizeof(small));
        ck(got == 4, "a long name is truncated to the buffer");
        eq(small, "ABCD", "keeping the front of it");
        ck(small[4] == 0, "and still terminated");
    }
    {
        // A one-byte buffer has room for the terminator and nothing else.
        const uint8_t ad[] = { 5, BT_AD_NAME_FULL, 'N','o','v','a' };
        char one[1];
        ck(bt_name_from_ad(ad, sizeof(ad), one, sizeof(one)) == 0, "a buffer of one holds no name");
        ck(one[0] == 0, "but is still terminated");
    }
    ck(bt_name_from_ad(nullptr, 0, nullptr, 0) == 0, "no buffer at all does not crash");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
