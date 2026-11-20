// base64: the RFC 4648 section 10 test vectors both ways, every length through
// a round trip, and the malformed inputs a decoder must refuse rather than
// guess at.
#include "b64.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }

int main(void) {
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *enc[]   = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char out[16];
        b64_encode((const uint8_t *)plain[i], (uint32_t)strlen(plain[i]), out);
        char what[64]; snprintf(what, sizeof(what), "RFC 4648 vector %d encodes", i);
        ck(!strcmp(out, enc[i]), what);
        uint8_t back[16];
        int32_t n = b64_decode(enc[i], (uint32_t)strlen(enc[i]), back, sizeof(back));
        snprintf(what, sizeof(what), "RFC 4648 vector %d decodes", i);
        ck(n == (int32_t)strlen(plain[i]) && !memcmp(back, plain[i], (size_t)n), what);
    }

    // Every length up to the link chunk and a bit past, every byte value.
    uint8_t src[800], back[800];
    char text[1100];
    for (uint32_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 37 + 11);
    bool all = true;
    for (uint32_t n = 0; n <= sizeof(src) && all; n++) {
        uint32_t tl = b64_encode(src, n, text);
        if (tl != b64_encoded_len(n)) all = false;
        int32_t m = b64_decode(text, tl, back, sizeof(back));
        if (m != (int32_t)n || memcmp(src, back, n)) all = false;
    }
    ck(all, "every length 0..800 round-trips and matches b64_encoded_len");
    ck(b64_encoded_len(768) == 1024, "a 768-byte chunk is 1024 characters");

    // Refusals.
    uint8_t o[16];
    ck(b64_decode("Zm9", 3, o, sizeof(o)) < 0, "a length that is not a multiple of 4");
    ck(b64_decode("Zm9*", 4, o, sizeof(o)) < 0, "a character outside the alphabet");
    ck(b64_decode("Zg==Zm8=", 8, o, sizeof(o)) < 0, "padding before the end");
    ck(b64_decode("Z===", 4, o, sizeof(o)) < 0, "too much padding");
    ck(b64_decode("Zm=v", 4, o, sizeof(o)) < 0, "a character after padding");
    ck(b64_decode("Zm9vYmFy", 8, o, 5) < 0, "output that would not fit");
    ck(b64_decode("Zm9vYmFy", 8, o, 6) == 6, "output that exactly fits");
    ck(b64_decode("Zm9v\n", 5, o, sizeof(o)) < 0, "a stray newline");

    printf("  b64: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
