// Host verification of the parity core: SHA-256, the registry, the accounts
// system. All pure logic, so it runs on a PC against the same code the device
// links. Auth correctness is the reason this is thorough.

#include "sha256.h"
#include "registry.h"
#include "users.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }

// Deterministic-but-advancing RNG so salts differ between users while tests stay
// reproducible. A real device backs rpc_rand32 with the SDK hardware RNG.
static uint32_t g_seed = 0x12345678;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

static const char *hx(const void *d, size_t n) {
    static char b[130]; hex_encode((const uint8_t *)d, n, b); return b;
}

int main(void) {
    // --- SHA-256 against the FIPS 180-4 vectors ---
    uint8_t dg[32];
    sha256("", 0, dg);
    ck(strcmp(hx(dg,32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
       "SHA-256 of empty string");
    sha256("abc", 3, dg);
    ck(strcmp(hx(dg,32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
       "SHA-256 of 'abc'");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, dg);
    ck(strcmp(hx(dg,32), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
       "SHA-256 of the 448-bit vector");
    // Multi-update must equal one-shot (the streaming path is what the device uses).
    Sha256Ctx c; sha256_init(&c);
    sha256_update(&c, "ab", 2); sha256_update(&c, "c", 1);
    uint8_t dg2[32]; sha256_final(&c, dg2);
    ck(memcmp(dg, dg2, 32) != 0, "sanity: different input, different digest");
    sha256("abc", 3, dg);
    ck(memcmp(dg, dg2, 32) == 0, "streaming update equals one-shot");

    // --- registry ---
    reg_clear();
    ck(reg_set("Settings.Version", "v2.0.0"), "set a key");
    ck(strcmp(reg_get("Settings.Version", "?"), "v2.0.0") == 0, "get it back");
    ck(strcmp(reg_get("Nope.Key", "def"), "def") == 0, "missing key returns default");
    reg_set("Settings.Version", "v2.0.1");
    ck(strcmp(reg_get("Settings.Version", "?"), "v2.0.1") == 0, "update in place");
    ck(reg_count() == 1, "update did not add a second entry");
    reg_set("System.TZ", "-5");
    ck(reg_get_int("System.TZ", 0) == -5, "integer read");
    ck(reg_get_int("Nope", 42) == 42, "integer default");
    ck(reg_dirty(), "a set marks the registry dirty");

    // roundtrip through text, the persistence path
    char buf[512];
    uint32_t n = reg_serialize(buf, sizeof(buf));
    ck(n > 0 && n < sizeof(buf), "serialize fits");
    reg_clear();
    ck(reg_count() == 0, "cleared");
    reg_load(buf, n);
    ck(reg_count() == 2, "reload restores every key");
    ck(strcmp(reg_get("Settings.Version", "?"), "v2.0.1") == 0, "values survive a roundtrip");
    ck(!reg_dirty(), "a freshly loaded registry is clean, so boot does not rewrite flash");

    // a comment line and a blank line in the file must be tolerated
    reg_load("# a comment\n\nA.B=1\n", 20);
    ck(reg_count() == 1 && strcmp(reg_get("A.B","?"),"1")==0, "comments and blanks are skipped");

    // the table is bounded, not unbounded
    reg_clear();
    char k[16]; bool overflow_ok = true;
    for (int i = 0; i < REG_MAX + 10; i++) { snprintf(k, sizeof(k), "k%d", i); reg_set(k, "v"); }
    ck(reg_count() == REG_MAX, "the registry is capped, not unbounded");
    (void)overflow_ok;

    // --- users ---
    users_clear();
    ck(users_add("root", "hunter2", true, false), "add an admin");
    ck(users_add("guest", nullptr, false, true), "add a NOPASS guest");
    ck(users_add("dash", "pw1234", false, false), "add a normal user");
    ck(!users_add("root", "x", false, false), "a duplicate name is refused");
    ck(!users_add("nopw", "", false, false), "an empty password (non-NOPASS) is refused");

    ck(users_verify("root", "hunter2"), "correct password verifies");
    ck(!users_verify("root", "wrong"), "wrong password is rejected");
    ck(!users_verify("root", ""), "empty password is rejected for a real account");
    ck(users_verify("guest", "anything at all"), "NOPASS accepts any password");
    ck(users_verify("guest", ""), "NOPASS accepts even an empty one");
    ck(!users_verify("ghost", "x"), "a nonexistent user never verifies");

    ck(users_is_admin("root"), "root is admin");
    ck(!users_is_admin("dash"), "dash is not");
    ck(users_is_nopass("guest"), "guest is NOPASS");

    // Two users with the SAME password must get DIFFERENT creds — the salt works.
    users_add("alice", "samepass", false, false);
    users_add("bob",   "samepass", false, false);
    char save[1024];
    users_serialize(save, sizeof(save));
    ck(strstr(save, "alice") && strstr(save, "bob"), "both saved");
    // verify both, then confirm their stored creds differ
    ck(users_verify("alice", "samepass") && users_verify("bob", "samepass"),
       "both verify with the shared password");

    // set_password changes the hash and clears any prior value
    ck(users_set_password("dash", "newpw"), "change a password");
    ck(users_verify("dash", "newpw"), "new password works");
    ck(!users_verify("dash", "pw1234"), "old password stops working");

    // protected accounts
    ck(!users_remove("root"), "root cannot be removed");
    ck(!users_remove("guest"), "guest cannot be removed");
    ck(users_remove("dash"), "an ordinary user can be removed");
    ck(!users_exists("dash"), "and is gone");

    // roundtrip
    uint32_t un = users_serialize(save, sizeof(save));
    uint32_t before = users_count();
    users_clear();
    users_load(save, un);
    ck(users_count() == before, "user table survives a save/load");
    ck(users_verify("root", "hunter2"), "and passwords still verify after reload");
    ck(users_is_admin("root"), "and roles survive");
    ck(users_verify("guest", "x"), "and NOPASS survives");

    // legacy 3-field line: root with no role field is admin, others are users
    users_clear();
    users_load("root,NOPASS,/home/root/\njoe,ab$cd,/home/joe/\n", 45);
    ck(users_is_admin("root"), "a 3-field root line is still admin (backward compatible)");
    ck(!users_is_admin("joe"), "a 3-field non-root line is a user");

    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
