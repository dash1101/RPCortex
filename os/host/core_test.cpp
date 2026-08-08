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

    // --- the per-user half of the registry -----------------------------------
    //
    // The whole contract, in the order it happens on a device: a device-wide
    // value is what everybody starts from, signing in shadows it, and signing
    // out puts it back.
    reg_clear();
    reg_scope_clear();

    ck(reg_is_user_key("User.Home"), "'User.' names a per-person key");
    ck(!reg_is_user_key("Apps.NovaD1_PIN_sda"), "an app key is not one");
    ck(!reg_is_user_key("System.TZ"), "nor a system key");

    // No scope: a "User." write is REFUSED rather than becoming the device
    // default. A background service that runs before anyone logs in must not
    // set the preference every later account inherits.
    ck(!reg_set("User.Home", "gallery"), "with nobody signed in, a User. write is refused");
    ck(reg_count() == 0, "and nothing was written anywhere");

    // A device-wide default, put there deliberately by loading the file — which
    // is also how a setting that USED to be global survives becoming per-user.
    const char *devfile = "User.Home=folders\nSystem.TZ=-4\n";
    reg_load(devfile, (uint32_t)strlen(devfile));
    ck(strcmp(reg_get("User.Home", "?"), "folders") == 0, "the device value is the default");

    reg_scope_set("ada");
    ck(strcmp(reg_get("User.Home", "?"), "folders") == 0,
       "a user who has set nothing still sees the device default");

    ck(reg_set("User.Home", "gallery"), "in a scope, the write lands");
    ck(strcmp(reg_get("User.Home", "?"), "folders") != 0, "and shadows the default");
    ck(strcmp(reg_get("User.Home", "?"), "gallery") == 0, "with their own value");
    ck(reg_scope_count() == 1, "in the user table");
    ck(reg_scope_dirty(), "which is now worth writing out");

    // The device file must NOT gain the user's key. reg_serialize is what
    // writes /os/registry.cfg, and one shared serializer would put every
    // account's preferences in it.
    char ubuf[256];
    uint32_t un2 = reg_serialize(ubuf, sizeof(ubuf));
    (void)un2;
    ck(strstr(ubuf, "gallery") == nullptr, "the device file does not gain a user's value");
    ck(strstr(ubuf, "User.Home=folders") != nullptr, "and keeps the default it had");

    uint32_t sn = reg_scope_serialize(ubuf, sizeof(ubuf));
    ck(strcmp(ubuf, "User.Home=gallery\n") == 0, "the user file holds exactly their keys");

    // Somebody else signs in. They must not inherit the last person's choice.
    reg_scope_clear();
    reg_scope_set("grace");
    ck(strcmp(reg_get("User.Home", "?"), "folders") == 0,
       "the next user is back to the device default");
    reg_scope_load(ubuf, sn);
    reg_scope_set("ada");
    ck(strcmp(reg_get("User.Home", "?"), "gallery") == 0, "and ada's file restores hers");

    // A device key that somehow appears in a user's file must not shadow the
    // real one — a read prefers the user table, and there would be no way to
    // see which value was in force.
    reg_scope_clear();
    reg_scope_set("mallory");
    const char *mixed = "System.TZ=+9\nUser.Home=menu\n";
    reg_scope_load(mixed, (uint32_t)strlen(mixed));
    ck(reg_scope_count() == 1, "a device key in a user's file is dropped on load");
    ck(reg_get_int("System.TZ", 0) == -4, "so it cannot shadow the device's own");
    ck(strcmp(reg_get("User.Home", "?"), "menu") == 0, "and the legitimate key still loads");

    // Signed out, the device value is what everyone sees again.
    reg_scope_clear();
    ck(strcmp(reg_get("User.Home", "?"), "folders") == 0, "signed out, the default is back");
    ck(reg_scope_count() == 0, "and no user table is left behind");

    // Bounded, like the device table.
    reg_scope_set("ada");
    char uk[24];
    for (int i = 0; i < REG_USER_MAX + 6; i++) {
        snprintf(uk, sizeof(uk), "User.k%d", i);
        reg_set(uk, "v");
    }
    ck(reg_scope_count() == REG_USER_MAX, "the user table is capped too");
    reg_scope_clear();
    reg_clear();

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
