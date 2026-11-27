#include "relsig.h"
#include "ed25519.h"
#include "b64.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

// The release keys (public halves; the private ones never leave the maintainer's
// machine). Primary signs every release; backup is kept offline for the day the
// primary is lost or replaced.
static const uint8_t kKeys[][32] = {
    // primary, id ffa56278
    {0x0e,0x71,0xaa,0x1e,0x30,0x17,0x47,0x6f,0xc1,0x64,0xf9,0xe5,0x6f,0x88,0x3b,0x4a,
     0xb6,0xdf,0xb0,0x13,0x75,0x23,0x26,0xa5,0x3c,0x35,0x80,0x3d,0x0b,0xdd,0xbe,0x67},
    // backup, id 05c654e8
    {0xba,0xee,0xc6,0x4b,0x21,0x48,0xdd,0xe3,0x87,0xe0,0x88,0xdd,0x62,0x1e,0x29,0x37,
     0x62,0x86,0x35,0x8c,0x1b,0x68,0xb4,0xab,0xe1,0xdd,0xee,0x55,0xfa,0x4b,0xf8,0x25},
};
static const unsigned kNKeys = sizeof(kKeys) / sizeof(kKeys[0]);

static void key_id(const uint8_t pk[32], char out[9]) {
    uint8_t d[32];
    sha256(pk, 32, d);
    for (int i = 0; i < 4; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

// The next space-separated word of *p into out (cap bytes with its 0). False
// when there is none, or it does not fit: a word that long is not ours.
static bool next_word(const char **p, char *out, size_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
    if (!n || n >= cap) return false;
    memcpy(out, s, n);
    out[n] = 0;
    *p = s + n;
    return true;
}

RelSig relsig_check(const uint8_t *manifest, size_t n, const char *sig_text, char which[9]) {
    if (which) which[0] = 0;
    if (!manifest || !sig_text) return RELSIG_MISSING;
    bool any_line = false, any_known = false;
    const char *p = sig_text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char line[160];
        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = 0;
            if (len && line[len - 1] == '\r') line[len - 1] = 0;
            // Split by hand, not sscanf: sscanf links newlib's float scanf, about
            // 12 KB (kernel.cpp and sys.cpp say the same), and this image has
            // never needed it.
            char kind[16], id[16], b64[128];
            const char *f = line;
            bool three = next_word(&f, kind, sizeof(kind)) && next_word(&f, id, sizeof(id)) &&
                         next_word(&f, b64, sizeof(b64));
            if (three && !strcmp(kind, "ed25519")) {
                any_line = true;
                uint8_t sig[66];
                int32_t sl = b64_decode(b64, (uint32_t)strlen(b64), sig, sizeof(sig));
                for (unsigned k = 0; k < kNKeys; k++) {
                    char kid[9];
                    key_id(kKeys[k], kid);
                    if (strcmp(kid, id) != 0) continue;
                    any_known = true;            // a known key's line, good or not
                    if (sl == 64 && ed25519_verify(sig, manifest, n, kKeys[k])) {
                        if (which) memcpy(which, kid, 9);
                        return RELSIG_OK;
                    }
                }
            }
        }
        if (!end) break;
        p = end + 1;
    }
    if (!any_line) return RELSIG_MISSING;
    return any_known ? RELSIG_BAD : RELSIG_UNKNOWN_KEY;
}

const char *relsig_str(RelSig r) {
    switch (r) {
        case RELSIG_OK:          return "signed by an RPCortex release key";
        case RELSIG_MISSING:     return "not signed";
        case RELSIG_UNKNOWN_KEY: return "signed by a key this firmware does not know";
        case RELSIG_BAD:         return "signed, but the signature does not match: the list was altered";
    }
    return "?";
}
