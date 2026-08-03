#include "repoindex.h"

#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ieq(const char *a, const char *b) {
    while (*a && *b) { if (lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

// Copy a JSON string body, resolving the escapes that actually appear in this
// index.
//
// `may_truncate` splits the two cases, and the split matters. A shortened URL
// points somewhere real and wrong, and a shortened version compares wrong, so
// those refuse and the entry is skipped. A shortened DESCRIPTION is just a
// shortened description — refusing there would make a package with a long blurb
// vanish from the index entirely, which is what the real repo index turned out
// to contain.
static bool copy_str(const char *src, uint32_t n, char *dst, uint32_t cap,
                     bool may_truncate = false) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            switch (src[i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                // \/ is legal and common in JSON-encoded URLs.
                default:  c = src[i]; break;
            }
        }
        if (o + 1 >= cap) {
            if (!may_truncate) return false;
            break;
        }
        dst[o++] = c;
    }
    dst[o] = 0;
    return true;
}

// Find the next `"` string starting at or after p, within end. Sets body/len to
// the contents. Returns the position just past the closing quote, or null.
static const char *next_string(const char *p, const char *end,
                               const char **body, uint32_t *len) {
    while (p < end && *p != '"') p++;
    if (p >= end) return nullptr;
    p++;                                  // past the opening quote
    const char *s = p;
    while (p < end) {
        if (*p == '\\') { p += 2; continue; }   // skip the escaped char
        if (*p == '"') break;
        p++;
    }
    if (p >= end) return nullptr;
    *body = s;
    *len = (uint32_t)(p - s);
    return p + 1;
}

uint32_t repo_walk(const char *json, uint32_t len, RepoEntryFn cb, void *ctx) {
    if (!json || !len) return 0;
    const char *p = json;
    const char *end = json + len;
    uint32_t seen = 0;

    // Enter the "packages" array. Without this the top-level "name" and
    // "maintainer" fields would be read as though they belonged to a package.
    const char *arr = nullptr;
    for (const char *q = json; q + 10 < end; q++) {
        if (!strncmp(q, "\"packages\"", 10)) { arr = q + 10; break; }
    }
    if (!arr) return 0;
    while (arr < end && *arr != '[') arr++;
    if (arr >= end) return 0;
    p = arr + 1;

    while (p < end) {
        while (p < end && *p != '{' && *p != ']') p++;
        if (p >= end || *p == ']') break;

        // Bound this object so a missing field cannot be read from the next one.
        const char *obj = p + 1;
        const char *close = obj;
        int depth = 1;
        while (close < end && depth) {
            if (*close == '"') {                 // skip strings wholesale
                close++;
                while (close < end && *close != '"') { if (*close == '\\') close++; close++; }
            } else if (*close == '{') depth++;
            else if (*close == '}') depth--;
            if (depth) close++;
        }
        if (close >= end) break;

        RepoEntry e{};
        bool bad = false;
        const char *q = obj;
        while (q < close) {
            const char *kb, *vb; uint32_t kn, vn;
            q = next_string(q, close, &kb, &kn);
            if (!q) break;
            // The value must be the next string in this object.
            const char *afterkey = q;
            while (afterkey < close && (*afterkey == ' ' || *afterkey == ':')) afterkey++;
            if (afterkey >= close || *afterkey != '"') {
                // A number, object or array. Only "size" is wanted, and only as
                // a sanity figure — the hash is what actually proves a download.
                if (kn == 4 && !strncmp(kb, "size", 4)) {
                    uint32_t v = 0;
                    const char *d = afterkey;
                    while (d < close && *d >= '0' && *d <= '9') { v = v * 10 + (uint32_t)(*d - '0'); d++; }
                    e.size = v;
                }
                continue;
            }
            q = next_string(afterkey, close, &vb, &vn);
            if (!q) break;

            char key[24];
            if (kn >= sizeof(key)) continue;
            memcpy(key, kb, kn); key[kn] = 0;

            if      (ieq(key, "name"))   { if (!copy_str(vb, vn, e.name,   sizeof(e.name)))   bad = true; }
            else if (ieq(key, "ver"))    { if (!copy_str(vb, vn, e.ver,    sizeof(e.ver)))    bad = true; }
            else if (ieq(key, "version")){ if (!copy_str(vb, vn, e.ver,    sizeof(e.ver)))    bad = true; }
            else if (ieq(key, "desc"))   { copy_str(vb, vn, e.desc, sizeof(e.desc), true); }
            else if (ieq(key, "author")) { if (!copy_str(vb, vn, e.author, sizeof(e.author))) bad = true; }
            else if (ieq(key, "url"))    { if (!copy_str(vb, vn, e.url,    sizeof(e.url)))    bad = true; }
            // A hash that does not fit is worse than no hash: it would compare
            // unequal against every download, making the package permanently
            // uninstallable for a reason nothing would explain.
            else if (ieq(key, "sha256")) { if (!copy_str(vb, vn, e.sha256, sizeof(e.sha256))) bad = true; }
            else if (ieq(key, "abi"))    { copy_str(vb, vn, e.abi,  sizeof(e.abi),  true); }
            else if (ieq(key, "arch"))   { copy_str(vb, vn, e.arch, sizeof(e.arch), true); }
        }

        // An entry with no name or no URL is not installable, and one whose
        // fields did not fit is not trustworthy. Skip rather than half-report.
        if (!bad && e.name[0] && e.url[0]) {
            seen++;
            if (cb && !cb(ctx, &e)) return seen;
        }
        p = close + 1;
    }
    return seen;
}

struct FindCtx { const char *want; RepoEntry *out; bool hit; };

static bool find_cb(void *ctx, const RepoEntry *e) {
    FindCtx *f = (FindCtx *)ctx;
    if (ieq(e->name, f->want)) { *f->out = *e; f->hit = true; return false; }
    return true;
}

bool repo_find(const char *json, uint32_t len, const char *name, RepoEntry *out) {
    FindCtx f{name, out, false};
    repo_walk(json, len, find_cb, &f);
    return f.hit;
}

int repo_version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); b++; }
        if (va != vb) return va < vb ? -1 : 1;
        // Step over one separator each. A component that simply ran out counts
        // as zero, so "2.1" and "2.1.0" compare equal.
        if (*a == '.') a++; else if (*a) return 1;   // trailing junk sorts newer
        if (*b == '.') b++; else if (*b) return -1;
    }
    return 0;
}
