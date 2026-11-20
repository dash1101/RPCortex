#include "sshkeys.h"
#include "sshcrypto.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// The largest key blob read from a file: an 8192-bit RSA key is 1047 bytes.
#define BLOB_MAX 1100

static bool is_space(char c) { return c == ' ' || c == '\t'; }

// The next line of `text` from *at, without its newline (and \r).
static bool next_line(const char *text, uint32_t len, uint32_t *at, const char **line, uint32_t *ln) {
    if (*at >= len) return false;
    uint32_t s = *at, e = s;
    while (e < len && text[e] != '\n') e++;
    *at = e < len ? e + 1 : e;
    uint32_t n = e - s;
    if (n && text[s + n - 1] == '\r') n--;
    *line = text + s;
    *ln = n;
    return true;
}

// The next whitespace-separated field.
static bool field(const char *l, uint32_t ln, uint32_t *at, const char **f, uint32_t *fn) {
    uint32_t i = *at;
    while (i < ln && is_space(l[i])) i++;
    if (i >= ln) return false;
    uint32_t s = i;
    while (i < ln && !is_space(l[i])) i++;
    *f = l + s; *fn = i - s; *at = i;
    return true;
}

static bool known_type(const char *t, uint32_t n) {
    return (n == 19 && !memcmp(t, "ecdsa-sha2-nistp256", 19)) || (n == 7 && !memcmp(t, "ssh-rsa", 7));
}

// The key on a "<type> <base64>" pair: decoded, and checked to name the same
// type inside as the word in front of it.
static uint32_t decode_key(const char *type, uint32_t tn, const char *b64, uint32_t bn,
                           uint8_t *out, uint32_t cap) {
    if (!known_type(type, tn)) return 0;
    uint32_t n = ssh_b64_decode(b64, bn, out, cap);
    if (n < 4) return 0;
    uint32_t inner = ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) | ((uint32_t)out[2] << 8) | out[3];
    if (inner != tn || n < 4 + tn || memcmp(out + 4, type, tn) != 0) return 0;
    return n;
}

bool ssh_authkeys_match(const char *text, uint32_t len, const uint8_t *blob, uint32_t n) {
    uint8_t *buf = (uint8_t *)malloc(BLOB_MAX);
    if (!buf) return false;
    bool found = false;
    uint32_t at = 0;
    const char *l; uint32_t ln;
    while (!found && next_line(text, len, &at, &l, &ln)) {
        uint32_t p = 0;
        const char *t, *b; uint32_t tn, bn;
        if (!field(l, ln, &p, &t, &tn) || t[0] == '#') continue;
        // An options field is anything that is not a key type in first place.
        if (!known_type(t, tn)) continue;
        if (!field(l, ln, &p, &b, &bn)) continue;
        uint32_t kn = decode_key(t, tn, b, bn, buf, BLOB_MAX);
        found = kn == n && memcmp(buf, blob, n) == 0;
    }
    free(buf);
    return found;
}

uint32_t ssh_pubkey_line(const uint8_t *blob, uint32_t n, const char *comment,
                         char *out, uint32_t cap) {
    char type[24];
    if (!ssh_key_type(blob, n, type, sizeof(type))) return 0;
    uint32_t tl = (uint32_t)strlen(type);
    if (cap < tl + 2) return 0;
    memcpy(out, type, tl);
    out[tl] = ' ';
    uint32_t b = ssh_b64_encode(blob, n, out + tl + 1, cap - tl - 1);
    if (!b) return 0;
    uint32_t len = tl + 1 + b;
    if (comment && comment[0]) {
        int w = snprintf(out + len, cap - len, " %s", comment);
        if (w < 0 || (uint32_t)w >= cap - len) return 0;
        len += (uint32_t)w;
    }
    return len;
}

static void host_pattern(const char *host, uint16_t port, char *out, uint32_t cap) {
    if (port == 22) snprintf(out, cap, "%s", host);
    else            snprintf(out, cap, "[%s]:%u", host, (unsigned)port);
}

static bool host_listed(const char *f, uint32_t fn, const char *want) {
    uint32_t wl = (uint32_t)strlen(want);
    uint32_t i = 0;
    while (i <= fn) {
        uint32_t j = i;
        while (j < fn && f[j] != ',') j++;
        // Case-insensitive, as host names are.
        if (j - i == wl) {
            bool eq = true;
            for (uint32_t k = 0; k < wl && eq; k++) {
                char a = f[i + k], b = want[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                eq = a == b;
            }
            if (eq) return true;
        }
        i = j + 1;
    }
    return false;
}

SshKnown ssh_known_hosts_check(const char *text, uint32_t len, const char *host, uint16_t port,
                               const uint8_t *blob, uint32_t n) {
    char want[96];
    host_pattern(host, port, want, sizeof(want));
    char mytype[24];
    if (!ssh_key_type(blob, n, mytype, sizeof(mytype))) return SSH_KH_UNKNOWN;
    uint8_t *buf = (uint8_t *)malloc(BLOB_MAX);
    if (!buf) return SSH_KH_UNKNOWN;
    SshKnown verdict = SSH_KH_UNKNOWN;
    uint32_t at = 0;
    const char *l; uint32_t ln;
    while (next_line(text, len, &at, &l, &ln)) {
        uint32_t p = 0;
        const char *h, *t, *b; uint32_t hn, tn, bn;
        if (!field(l, ln, &p, &h, &hn) || h[0] == '#' || h[0] == '|' || h[0] == '@') continue;
        if (!field(l, ln, &p, &t, &tn) || !field(l, ln, &p, &b, &bn)) continue;
        if (!host_listed(h, hn, want)) continue;
        uint32_t kn = decode_key(t, tn, b, bn, buf, BLOB_MAX);
        if (!kn) continue;
        if (kn == n && memcmp(buf, blob, n) == 0) { verdict = SSH_KH_MATCH; break; }
        // Same host, same kind of key, different key: the warning case.
        if (tn == strlen(mytype) && !memcmp(t, mytype, tn)) verdict = SSH_KH_CHANGED;
    }
    free(buf);
    return verdict;
}

uint32_t ssh_known_hosts_line(const char *host, uint16_t port, const uint8_t *blob, uint32_t n,
                              char *out, uint32_t cap) {
    char hp[96];
    host_pattern(host, port, hp, sizeof(hp));
    uint32_t hl = (uint32_t)strlen(hp);
    if (cap < hl + 3) return 0;
    memcpy(out, hp, hl);
    out[hl] = ' ';
    uint32_t k = ssh_pubkey_line(blob, n, nullptr, out + hl + 1, cap - hl - 2);
    if (!k) return 0;
    uint32_t len = hl + 1 + k;
    out[len++] = '\n';
    out[len] = 0;
    return len;
}
