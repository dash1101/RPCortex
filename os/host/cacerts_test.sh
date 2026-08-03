#!/usr/bin/env bash
# Does the shipped CA bundle actually parse, with the DEVICE's mbedtls config?
#
# This exists because it did not, and nothing said so. ISRG Root X2 is signed
# with ecdsa-with-SHA384, mbedtls 3.x gates SHA-384 behind its own flag that the
# SDK configuration does not set, and lwIP treats ANY non-zero parse result as
# total failure. So one unparseable certificate out of three made HTTPS
# impossible, and the only symptom was a package manager that would not verify.
#
# Compiling mbedtls from source with the real config is the only honest test:
# the host's own mbedtls has a different configuration and would happily parse
# a bundle the device cannot.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OS="$HERE/.."
SDK="${PICO_SDK_PATH:-$OS/../sdk}"
BUNDLE="$OS/ca_certs.pem"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -d "$SDK/lib/mbedtls/library" ]; then
    echo "  SKIP cacerts (mbedtls not checked out at $SDK/lib/mbedtls)"
    exit 0
fi

cat > "$TMP/t.c" <<'CEOF'
#include "mbedtls/x509_crt.h"
#include <stdio.h>
#include <stdlib.h>
int mbedtls_hardware_poll(void *d, unsigned char *o, size_t l, size_t *ol) {
    (void)d; for (size_t i = 0; i < l; i++) o[i] = (unsigned char)i; *ol = l; return 0;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) return 2;
    b[n] = 0; fclose(f);
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    // n + 1: mbedtls counts the terminator for PEM, exactly as the device does.
    int rc = mbedtls_x509_crt_parse(&crt, b, n + 1);
    int count = 0;
    for (mbedtls_x509_crt *c = &crt; c && c->version; c = c->next) count++;
    printf("  %d certificate(s), parse returned %d\n", count, rc);
    // Non-zero means at least one failed, and lwIP refuses the whole bundle.
    if (rc != 0) { printf("    FAIL %d certificate(s) did not parse\n", rc); return 1; }
    if (count < 1) { printf("    FAIL no certificates in the bundle\n"); return 1; }
    return 0;
}
CEOF

gcc -w -o "$TMP/t" "$TMP/t.c" \
    -DMBEDTLS_CONFIG_FILE="\"$OS/mbedtls_config.h\"" \
    -I "$SDK/lib/mbedtls/include" -I "$SDK/src/rp2_common/pico_mbedtls/include" \
    "$SDK"/lib/mbedtls/library/*.c 2>/dev/null
"$TMP/t" "$BUNDLE"
