// Signed releases (core/relsig.h): SHA-512 against FIPS 180-4's examples,
// Ed25519 against RFC 8032's test vectors, and a release list signed with the
// real primary key by openssl (what tools/release.py does) verified by the
// board's code, with every way it should be refused.
#include "../core/relsig.h"
#include "../core/sha512.h"
#include "../core/ed25519.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_checks, g_fail;
#define CK(c, what) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static std::vector<uint8_t> unhex(const char *h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
        unsigned x; sscanf(h + i, "%2x", &x); v.push_back((uint8_t)x);
    }
    return v;
}
static std::string hex(const uint8_t *d, size_t n) {
    std::string s; char b[3];
    for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", d[i]); s += b; }
    return s;
}

// relsig_test <manifest> <signature file>: check one pair with the board's code
// and print the verdict (tools/release.py's output, checked end to end).
static int check_files(const char *mpath, const char *spath) {
    std::string m, sg;
    FILE *f = fopen(mpath, "rb"); if (!f) return 2;
    char b[4096]; size_t k;
    while ((k = fread(b, 1, sizeof(b), f)) > 0) m.append(b, k);
    fclose(f);
    f = fopen(spath, "rb"); if (!f) return 2;
    while ((k = fread(b, 1, sizeof(b), f)) > 0) sg.append(b, k);
    fclose(f);
    char which[9];
    RelSig r = relsig_check((const uint8_t *)m.data(), m.size(), sg.c_str(), which);
    printf("%s%s%s\n", relsig_str(r), r == RELSIG_OK ? ", key " : "", r == RELSIG_OK ? which : "");
    return r == RELSIG_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3) return check_files(argv[1], argv[2]);
    // --- SHA-512 -----------------------------------------------------------------
    uint8_t d[64]; Sha512Ctx c;
    sha512_init(&c); sha512_update(&c, "abc", 3); sha512_final(&c, d);
    CK(hex(d, 64) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "sha512(\"abc\")");
    sha512_init(&c); sha512_final(&c, d);
    CK(hex(d, 64) == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                     "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "sha512 of nothing");
    const char *two = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512_init(&c); sha512_update(&c, two, strlen(two)); sha512_final(&c, d);
    CK(hex(d, 64) == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                     "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909", "sha512, two blocks");
    // Fed a byte at a time: the same answer.
    sha512_init(&c); for (size_t i = 0; i < strlen(two); i++) sha512_update(&c, two + i, 1); sha512_final(&c, d);
    CK(hex(d, 64).substr(0, 16) == "8e959b75dae313da", "sha512 a byte at a time");

    // --- Ed25519, RFC 8032 section 7.1 -------------------------------------------
    auto pk1 = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto sg1 = unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                     "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CK(ed25519_verify(sg1.data(), (const uint8_t *)"", 0, pk1.data()), "RFC 8032 test 1 (empty message)");
    auto pk2 = unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    auto ms2 = unhex("72");
    auto sg2 = unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    CK(ed25519_verify(sg2.data(), ms2.data(), 1, pk2.data()), "RFC 8032 test 2 (one byte)");
    sg2[10] ^= 1;
    CK(!ed25519_verify(sg2.data(), ms2.data(), 1, pk2.data()), "a changed signature is refused");

    // --- a release list signed by tools/release.py's way (openssl, the primary key)
    const char *manifest = "{\n  \"packages\": [\n    {\"name\": \"pico2_w\", \"ver\": \"2.0.999\", \"sha256\": \"00\"}\n  ]\n}\n";
    const char *sig = "ed25519 ffa56278 2cH/rr2IJIqAtRkzow7LUBsLgNTHcrtSvVgOOBc67EqkDhUnFKeKYAVC5WE42SX20Eb4M4q/mZqQu0HFO1+2Bw==\n";
    char which[9];
    const uint8_t *m = (const uint8_t *)manifest;
    size_t n = strlen(manifest);
    CK(relsig_check(m, n, sig, which) == RELSIG_OK && !strcmp(which, "ffa56278"), "the primary key's signature verifies");

    std::string changed(manifest);
    changed[changed.find("999")] = '8';
    CK(relsig_check((const uint8_t *)changed.data(), changed.size(), sig, which) == RELSIG_BAD,
       "a list changed after signing is refused as altered");
    CK(relsig_check(m, n, "", which) == RELSIG_MISSING, "no signature: missing");
    CK(relsig_check(m, n, "# nothing here\n", which) == RELSIG_MISSING, "a file with no signature line: missing");
    CK(relsig_check(m, n, "ed25519 deadbeef 2cH/rr2IJIqAtRkzow7LUBsLgNTHcrtSvVgOOBc67EqkDhUnFKeKYAVC5WE42SX20Eb4M4q/mZqQu0HFO1+2Bw==\n", which)
       == RELSIG_UNKNOWN_KEY, "a signature from a key the firmware does not carry");
    std::string two_lines = std::string("ed25519 deadbeef AAAA\n") + sig;
    CK(relsig_check(m, n, two_lines.c_str(), which) == RELSIG_OK, "one good line among others is enough");
    CK(relsig_check(m, n, "ed25519 ffa56278 !!!notbase64!!!\n", which) == RELSIG_BAD,
       "garbage under a known key's id: refused");
    CK(relsig_check(m, n - 1, sig, which) == RELSIG_BAD, "one byte short: refused");

    printf("relsig_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
