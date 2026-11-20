#include "sshwire.h"

#include <string.h>

void sshw_init(SshW *w, uint8_t *buf, uint32_t cap) {
    w->p = buf; w->cap = cap; w->len = 0; w->err = false;
}

static bool room(SshW *w, uint32_t n) {
    if (w->err) return false;
    // Two comparisons rather than len + n > cap: the sum can wrap for a huge n.
    if (n > w->cap || w->len > w->cap - n) { w->err = true; return false; }
    return true;
}

void sshw_u8(SshW *w, uint8_t v) {
    if (!room(w, 1)) return;
    w->p[w->len++] = v;
}

void sshw_bool(SshW *w, bool v) { sshw_u8(w, v ? 1 : 0); }

void sshw_u32(SshW *w, uint32_t v) {
    if (!room(w, 4)) return;
    w->p[w->len++] = (uint8_t)(v >> 24);
    w->p[w->len++] = (uint8_t)(v >> 16);
    w->p[w->len++] = (uint8_t)(v >> 8);
    w->p[w->len++] = (uint8_t)v;
}

void sshw_raw(SshW *w, const void *data, uint32_t n) {
    if (!room(w, n)) return;
    if (n) memmove(w->p + w->len, data, n);
    w->len += n;
}

void sshw_string(SshW *w, const void *data, uint32_t n) {
    sshw_u32(w, n);
    sshw_raw(w, data, n);
}

void sshw_cstr(SshW *w, const char *s) { sshw_string(w, s, (uint32_t)strlen(s)); }

void sshw_mpint(SshW *w, const uint8_t *be, uint32_t n) {
    while (n && be[0] == 0) { be++; n--; }
    bool pad = n && (be[0] & 0x80);
    sshw_u32(w, n + (pad ? 1 : 0));
    if (pad) sshw_u8(w, 0);
    sshw_raw(w, be, n);
}

// --- reading ------------------------------------------------------------------

void sshr_init(SshR *r, const uint8_t *buf, uint32_t len) {
    r->p = buf; r->len = len; r->pos = 0; r->err = false;
}

uint32_t sshr_left(const SshR *r) { return r->err ? 0 : r->len - r->pos; }

static bool have(SshR *r, uint32_t n) {
    if (r->err) return false;
    if (n > r->len - r->pos) { r->err = true; return false; }
    return true;
}

uint8_t sshr_u8(SshR *r) {
    if (!have(r, 1)) return 0;
    return r->p[r->pos++];
}

// RFC 4251: any non-zero byte is TRUE.
bool sshr_bool(SshR *r) { return sshr_u8(r) != 0; }

uint32_t sshr_u32(SshR *r) {
    if (!have(r, 4)) return 0;
    const uint8_t *b = r->p + r->pos;
    r->pos += 4;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

bool sshr_string(SshR *r, const uint8_t **data, uint32_t *n) {
    uint32_t len = sshr_u32(r);
    if (!have(r, len)) { *data = nullptr; *n = 0; return false; }
    *data = r->p + r->pos;
    *n = len;
    r->pos += len;
    return true;
}

bool sshr_cstr(SshR *r, char *out, uint32_t cap) {
    const uint8_t *d; uint32_t n;
    if (cap) out[0] = 0;
    if (!sshr_string(r, &d, &n)) return false;
    if (n >= cap || memchr(d, 0, n)) { r->err = true; return false; }
    memcpy(out, d, n);
    out[n] = 0;
    return true;
}

bool sshr_mpint(SshR *r, const uint8_t **mag, uint32_t *n) {
    const uint8_t *d; uint32_t len;
    if (!sshr_string(r, &d, &len)) return false;
    if (len && (d[0] & 0x80)) { r->err = true; return false; }   // negative
    while (len && d[0] == 0) { d++; len--; }
    *mag = d; *n = len;
    return true;
}

// --- name-lists -----------------------------------------------------------------

bool ssh_namelist_has(const uint8_t *list, uint32_t n, const char *name) {
    uint32_t want = (uint32_t)strlen(name);
    uint32_t i = 0;
    while (i <= n) {
        uint32_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == want && memcmp(list + i, name, want) == 0) return true;
        i = j + 1;
    }
    return false;
}

bool ssh_negotiate(const uint8_t *client, uint32_t cn, const uint8_t *server, uint32_t sn,
                   char *out, uint32_t cap) {
    uint32_t i = 0;
    char name[96];
    while (i < cn) {
        uint32_t j = i;
        while (j < cn && client[j] != ',') j++;
        uint32_t len = j - i;
        if (len && len < sizeof(name)) {
            memcpy(name, client + i, len);
            name[len] = 0;
            if (ssh_namelist_has(server, sn, name)) {
                if (len >= cap) return false;
                memcpy(out, name, len + 1);
                return true;
            }
        }
        i = j + 1;
    }
    return false;
}

bool ssh_namelist_first(const uint8_t *list, uint32_t n, char *out, uint32_t cap) {
    uint32_t j = 0;
    while (j < n && list[j] != ',') j++;
    if (j >= cap) return false;
    memcpy(out, list, j);
    out[j] = 0;
    return true;
}
