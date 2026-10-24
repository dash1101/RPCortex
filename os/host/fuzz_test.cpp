// Mutation fuzzing for the parsers that read bytes somebody else wrote.
//
// A valid input is copied, damaged a few bytes at a time, and fed to the parser.
// The pass condition is that nothing misbehaves: the host runs under AddressSan
// and UndefinedBehaviourSan with halt-on-error, so an out-of-bounds read, a
// signed overflow or a misaligned load aborts the run with a stack. Each target
// also checks the few things its API promises (a count never exceeds what it was
// given, a string is terminated inside its buffer, two versions compare
// consistently both ways round) and runs against a work budget, so a parser that
// can be sent round a loop forever fails instead of hanging the suite.
//
// WHAT IT READS. An HTTP response comes from whatever answers the connection. A
// package index comes from a repository that may be someone else's. A FAT volume
// comes from a card or from a PC writing to the USB drive. None of those is
// trusted, and none of them has a debugger on the other end.
//
// Deterministic: a fixed seed, so a failure repeats. RPC_FUZZ_SEED picks another
// and RPC_FUZZ_ITERS a longer run (the default keeps the suite quick).
//
//     RPC_FUZZ_ITERS=2000000 RPC_FUZZ_SEED=7 ./fuzz_test

#include "httpparse.h"
#include "repoindex.h"
#include "pkgindex.h"
#include "fatro.h"
#include "cmdline.h"
#include "lineedit.h"
#include "clockcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <signal.h>
#include <unistd.h>

typedef std::vector<uint8_t> Bytes;

static int checks = 0, fails = 0;
static void ck(bool c, const char *what, uint32_t iter) {
    checks++;
    if (!c) { fails++; if (fails < 20) printf("  FAIL: %s  (iteration %u)\n", what, iter); }
}

// --- the generator ------------------------------------------------------------

static uint64_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

static const uint8_t kInteresting[] = {
    0x00, 0x01, 0x7f, 0x80, 0xff, ' ', '\r', '\n', '\t', '"', '\'', '\\', '/', ':', ';', ',',
    '{', '}', '[', ']', '<', '>', '=', '&', '|', '%', '-', '+', '.', '0', '9', 'a', 'f', 'x',
    0x1b, 0x03, 0x08,
};

static void mutate(Bytes &b, uint32_t max_len) {
    uint32_t ops = 1 + below(4);
    while (ops--) {
        uint32_t n = (uint32_t)b.size();
        switch (below(9)) {
        case 0: if (n) b[below(n)] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (n) b[below(n)] = (uint8_t)rnd(); break;
        case 2: if (n) b[below(n)] = kInteresting[below(sizeof(kInteresting))]; break;
        case 3: {                                           // insert a few bytes
            uint32_t at = below(n + 1), k = 1 + below(8);
            if (n + k > max_len) break;
            Bytes ins(k);
            for (auto &x : ins) x = below(2) ? (uint8_t)rnd() : kInteresting[below(sizeof(kInteresting))];
            b.insert(b.begin() + at, ins.begin(), ins.end());
            break;
        }
        case 4: if (n > 1) {                                // delete a run
            uint32_t at = below(n), k = 1 + below(n - at < 16 ? n - at : 16);
            b.erase(b.begin() + at, b.begin() + at + k);
        }
            break;
        case 5: if (n > 2) b.resize(1 + below(n - 1)); break;   // truncate
        case 6: if (n > 2) {                                // duplicate a run
            uint32_t at = below(n), k = 1 + below(n - at < 32 ? n - at : 32);
            if (n + k > max_len) break;
            Bytes run(b.begin() + at, b.begin() + at + k);
            b.insert(b.begin() + below(n + 1), run.begin(), run.end());
        }
            break;
        case 7: if (n >= 4) {                               // a 32-bit boundary value
            static const uint32_t v[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu,
                                         0xffff, 0x10000, 0xfff, 0xff8, 0x0fffffffu};
            uint32_t x = v[below(sizeof(v) / sizeof(v[0]))], at = below(n - 3);
            for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(x >> (8 * i));
        }
            break;
        default: if (n > 1) {                               // swap two bytes
            uint32_t i = below(n), j = below(n);
            uint8_t t = b[i]; b[i] = b[j]; b[j] = t;
        }
            break;
        }
        if (b.empty()) b.push_back((uint8_t)rnd());
    }
}

static Bytes S(const char *s) { return Bytes((const uint8_t *)s, (const uint8_t *)s + strlen(s)); }

static Bytes pick(const std::vector<Bytes> &corpus) { return corpus[below((uint32_t)corpus.size())]; }

// --- HTTP ----------------------------------------------------------------------

static int count_sink(void *ctx, const uint8_t *, uint32_t len) {
    *(uint64_t *)ctx += len;
    return 0;
}
static int refuse_sink(void *ctx, const uint8_t *, uint32_t) { (*(uint64_t *)ctx)++; return 1; }
static void header_noop(void *ctx, const char *line) {
    (*(uint32_t *)ctx)++;
    (void)line;
}

static void fuzz_http(uint32_t iters) {
    std::vector<Bytes> corpus = {
        S("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world"),
        S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"),
        S("HTTP/1.1 301 Moved\r\nLocation: https://example.org/new/path?x=1\r\nContent-Length: 0\r\n\r\n"),
        S("HTTP/1.0 200 OK\r\nServer: x\r\n\r\nbody until the connection closes"),
        S("HTTP/1.1 302 Found\r\nlocation: /elsewhere\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Trailer: 1\r\n\r\n"),
        S("HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope"),
        S("HTTP/1.1 200 OK\r\nContent-Length: 2147483648\r\n\r\nx"),
        S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffff\r\nx"),
        S("HTTP/1.1 206 Partial\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd"),
    };
    // Lines at and past the limits: a header, a Location and a chunk-size line
    // just under, exactly at and well over the buffer they land in.
    for (uint32_t n : {HTTP_LINE_MAX - 30u, HTTP_LINE_MAX - 2u, (uint32_t)HTTP_LINE_MAX, HTTP_LINE_MAX + 40u, 700u}) {
        std::string pad(n, 'a');
        corpus.push_back(S(("HTTP/1.1 200 OK\r\nX-Pad: " + pad + "\r\nContent-Length: 2\r\n\r\nhi").c_str()));
        corpus.push_back(S(("HTTP/1.1 302 Found\r\nLocation: /" + pad + "\r\n\r\n").c_str()));
        corpus.push_back(S(("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;" + pad + "\r\nhello\r\n0\r\n\r\n").c_str()));
        corpus.push_back(S(("HTTP/1.1 200 " + pad + "\r\n\r\n").c_str()));
    }
    HttpUrl base;
    http_parse_url("http://example.com/dir/file", &base);

    for (uint32_t it = 0; it < iters; it++) {
        Bytes in = pick(corpus);
        mutate(in, 4096);

        HttpParser p;
        uint64_t sunk = 0;
        uint32_t headers = 0;
        bool refuse = (it % 11) == 0;
        http_parser_init(&p, refuse ? refuse_sink : count_sink, &sunk);
        p.head_only = (it % 7) == 0;
        if (it % 3 == 0) { p.header_cb = header_noop; p.header_ctx = &headers; }

        uint64_t consumed = 0;
        size_t at = 0;
        while (at < in.size()) {
            uint32_t piece = 1 + below(40);
            if (piece > in.size() - at) piece = (uint32_t)(in.size() - at);
            uint32_t used = http_parser_feed(&p, in.data() + at, piece);
            ck(used <= piece, "feed consumed more than it was given", it);
            consumed += used;
            at += piece;
            if (http_parser_done(&p) || http_parser_failed(&p)) break;
        }
        http_parser_eof(&p);
        ck(p.state <= HTTP_ST_ERROR, "parser state out of range", it);
        ck(p.line_len < HTTP_LINE_MAX, "header line length past its buffer", it);
        ck(memchr(p.location, 0, sizeof(p.location)) != nullptr, "Location not terminated", it);
        ck(p.body_seen >= 0, "negative body count", it);
        ck(!refuse ? (uint64_t)p.body_seen <= consumed : true, "more body than bytes fed", it);
        ck(!http_parser_done(&p) || !http_parser_failed(&p), "done and failed at once", it);

        if (http_is_redirect(&p)) {
            HttpUrl to;
            if (http_resolve_redirect(&base, p.location, &to)) {
                ck(to.host[0] != 0, "redirect resolved to an empty host", it);
                ck(to.path[0] == '/', "redirect path does not begin with /", it);
                ck(to.port != 0, "redirect resolved to port 0", it);
            }
        }
    }
}

static void fuzz_url(uint32_t iters) {
    std::vector<Bytes> corpus = {
        S("http://example.com/path?x=1"), S("https://host:8443/"), S("http://[::1]:80/a"),
        S("https://user@host/a/b/../c"), S("http://h"), S("http://h:/x"), S("HTTP://UP.CASE/X"),
        S("http://host:65535/p"), S("http://host:65536/p"), S("ftp://nope/"),
    };
    // Hosts and paths just under, at and over the buffers they are copied into.
    for (uint32_t n : {HTTP_HOST_MAX - 2u, HTTP_HOST_MAX - 1u, (uint32_t)HTTP_HOST_MAX, HTTP_HOST_MAX + 1u}) {
        corpus.push_back(S(("http://" + std::string(n, 'h') + "/x").c_str()));
        corpus.push_back(S(("https://" + std::string(n, 'h')).c_str()));
    }
    for (uint32_t n : {HTTP_PATH_MAX - 2u, HTTP_PATH_MAX - 1u, (uint32_t)HTTP_PATH_MAX, HTTP_PATH_MAX + 1u}) {
        corpus.push_back(S(("http://h/" + std::string(n, 'p')).c_str()));
        corpus.push_back(S(("/" + std::string(n, 'p')).c_str()));
    }
    corpus.push_back(S("http://h:99999999999999999999/"));
    corpus.push_back(S("http://h:4294967297/"));
    HttpUrl base;
    http_parse_url("https://example.com:8443/a/b/c?q", &base);
    for (uint32_t it = 0; it < iters; it++) {
        Bytes in = pick(corpus);
        mutate(in, 600);
        in.push_back(0);
        HttpUrl u;
        if (http_parse_url((const char *)in.data(), &u)) {
            ck(u.host[0] != 0, "parsed URL with an empty host", it);
            ck(u.path[0] == '/', "parsed path does not begin with /", it);
            ck(u.port != 0, "parsed port 0", it);
            ck(memchr(u.host, 0, sizeof(u.host)) && memchr(u.path, 0, sizeof(u.path)),
               "URL fields not terminated", it);
        }
        HttpUrl to;
        if (http_resolve_redirect(&base, (const char *)in.data(), &to)) {
            ck(to.host[0] != 0 && to.path[0] == '/', "redirect to a malformed target", it);
            ck(memchr(to.host, 0, sizeof(to.host)) && memchr(to.path, 0, sizeof(to.path)),
               "redirect fields not terminated", it);
        }
    }
}

// --- package index ---------------------------------------------------------------

struct RepoCtx { uint32_t seen; bool ok; std::vector<std::string> vers; };

static bool repo_cb(void *ctx, const RepoEntry *e) {
    RepoCtx *c = (RepoCtx *)ctx;
    c->seen++;
    if (!memchr(e->name, 0, sizeof(e->name)) || !memchr(e->ver, 0, sizeof(e->ver)) ||
        !memchr(e->desc, 0, sizeof(e->desc)) || !memchr(e->author, 0, sizeof(e->author)) ||
        !memchr(e->url, 0, sizeof(e->url)) || !memchr(e->sha256, 0, sizeof(e->sha256)) ||
        !memchr(e->abi, 0, sizeof(e->abi)) || !memchr(e->arch, 0, sizeof(e->arch)) ||
        !memchr(e->kind, 0, sizeof(e->kind)))
        c->ok = false;
    if (c->vers.size() < 6) c->vers.push_back(e->ver);
    return true;
}

static Bytes read_file(const char *path) {
    Bytes b;
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return b;
}

static void fuzz_repo(uint32_t iters) {
    std::vector<Bytes> corpus;
    Bytes a = read_file("fixtures/index.json"), b = read_file("fixtures/index-v2.json");
    if (!a.empty()) corpus.push_back(a);
    if (!b.empty()) corpus.push_back(b);
    corpus.push_back(S("{\"packages\":[{\"name\":\"a\",\"ver\":\"1.0\",\"url\":\"http://x/y\","
                       "\"sha256\":\"00\",\"size\":12,\"arch\":\"armv8m\"}]}"));
    corpus.push_back(S("{\"packages\":[{\"name\":\"a\\\"b\",\"desc\":\"x\\\\y\\u0041\"},{}]}"));
    ck(corpus.size() >= 3, "no index fixtures found (run from os/host)", 0);

    const char *vers[] = {"1.0", "1.10.0", "1.9", "2", "0.0.1", "", "1.", ".1", "99999999999", "1..2", "a.b"};
    for (uint32_t it = 0; it < iters; it++) {
        Bytes in = pick(corpus);
        mutate(in, 16384);
        in.push_back(0);
        uint32_t len = (uint32_t)in.size() - 1;
        RepoCtx c{0, true, {}};
        uint32_t n = repo_walk((const char *)in.data(), len, repo_cb, &c);
        ck(n == c.seen, "repo_walk count disagrees with the callbacks", it);
        ck(c.ok, "a RepoEntry string was not terminated", it);
        RepoEntry e;
        repo_find((const char *)in.data(), len, "a", &e);

        // Two versions compare the same way round, and every version equals itself.
        for (const std::string &v : c.vers) {
            ck(repo_version_cmp(v.c_str(), v.c_str()) == 0, "a version differs from itself", it);
            const char *o = vers[below(sizeof(vers) / sizeof(vers[0]))];
            int ab = repo_version_cmp(v.c_str(), o), ba = repo_version_cmp(o, v.c_str());
            ck((ab > 0) == (ba < 0) && (ab == 0) == (ba == 0), "version order is not antisymmetric", it);
        }
    }
}

// --- the installed-package list ------------------------------------------------------

struct PkgCtx { uint32_t seen; };
static void pkg_cb(void *ctx, const char *name, const char *ver) {
    ((PkgCtx *)ctx)->seen++;
    (void)name; (void)ver;
}

static void fuzz_pkgindex(uint32_t iters) {
    std::vector<Bytes> corpus = {
        S("greet,1.0\nbench,1.2.3\nnovad1,0.9\n"), S("a,\nb,c\n\n,d,\n"), S("x,1\r\ny,2\r\n"),
        S(""), S("name_only"), S("a,1\nb,2"), S("bench,1\nbench,2\nbenchmark,3\n"),
    };
    for (uint32_t it = 0; it < iters; it++) {
        Bytes in = pick(corpus);
        mutate(in, 600);
        const uint32_t cap = 640;
        char buf[cap];
        uint32_t len = (uint32_t)in.size() < cap - 1 ? (uint32_t)in.size() : cap - 2;
        memcpy(buf, in.data(), len);
        buf[len] = 0;

        PkgCtx c{0};
        pkgindex_walk(buf, len, pkg_cb, &c);
        pkgindex_has(buf, len, "bench");
        uint32_t nl = pkgindex_remove(buf, len, cap, "bench");
        ck(nl <= len, "remove grew the list", it);
        ck(nl < cap && buf[nl] == 0, "remove left the list unterminated", it);
        nl = pkgindex_add(buf, nl, cap, "added", "1.0");
        ck(nl < cap && buf[nl] == 0, "add ran past the buffer", it);
        ck(pkgindex_has(buf, nl, "added") || nl + 1 >= cap, "an added package is not found", it);
    }
}

// --- a FAT volume -----------------------------------------------------------------------
//
// Three seed volumes — FAT12, FAT16 and FAT32 — because the format is decided by
// the cluster count and each width has its own table arithmetic (FAT12's entries
// straddle sectors). Each carries a long-name chain, a file in one cluster, a
// file in two, and a subdirectory. They are SPARSE: only the sectors something
// was written to exist, the rest read back as zeros, so a 70,000-sector volume
// costs a dozen sectors.

namespace {

typedef std::array<uint8_t, FATRO_SECTOR> Sec;
typedef std::map<uint32_t, Sec> Sectors;

struct Image {
    const Sectors *seed = nullptr;
    Sectors over;                       // sectors this run damaged
    uint64_t reads = 0, budget = 0;
    bool over_budget = false;
    const FatRo *geom = nullptr;        // set once mounted
    bool escaped = false;               // a read went outside the volume the header describes
};

bool img_read(void *ctx, uint32_t lba, void *buf) {
    Image *im = (Image *)ctx;
    if (++im->reads > im->budget) { im->over_budget = true; return false; }
    // A reader must stay inside the volume its own boot sector describes. Beyond
    // the end is another partition or nothing, and here it would read as zeros,
    // which is why nothing else would notice.
    if (im->geom && im->geom->mounted) {
        uint64_t end = (uint64_t)im->geom->first_data + (uint64_t)im->geom->clusters * im->geom->spc;
        if (lba >= end) im->escaped = true;
    }
    auto o = im->over.find(lba);
    if (o != im->over.end()) { memcpy(buf, o->second.data(), FATRO_SECTOR); return true; }
    auto s = im->seed->find(lba);
    if (s != im->seed->end()) { memcpy(buf, s->second.data(), FATRO_SECTOR); return true; }
    memset(buf, 0, FATRO_SECTOR);
    return true;
}

void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void w32(uint8_t *p, uint32_t v) { w16(p, v & 0xffff); w16(p + 2, v >> 16); }

struct Builder {
    Sectors s;
    int type = 12;
    uint32_t spc = 1, rsvd = 1, nfats = 2, root_ents = 16, fatsz = 1, total = 64;
    uint32_t first_fat = 1, first_root = 3, first_data = 4, root_cluster = 0;

    uint8_t *at(uint32_t lba) {
        auto it = s.find(lba);
        if (it == s.end()) { Sec z; z.fill(0); it = s.emplace(lba, z).first; }
        return it->second.data();
    }
    uint32_t lba_of(uint32_t cl) const { return first_data + (cl - 2) * spc; }

    void fat_set(uint32_t cl, uint32_t val) {
        for (uint32_t copy = 0; copy < nfats; copy++) {
            uint32_t base = first_fat + copy * fatsz;
            if (type == 12) {
                uint32_t off = cl + cl / 2;
                uint8_t *a = at(base + off / FATRO_SECTOR), *b = at(base + (off + 1) / FATRO_SECTOR);
                uint16_t cur = (uint16_t)(a[off % FATRO_SECTOR] | (b[(off + 1) % FATRO_SECTOR] << 8));
                cur = (cl & 1) ? (uint16_t)((cur & 0x000F) | (val << 4)) : (uint16_t)((cur & 0xF000) | (val & 0xFFF));
                a[off % FATRO_SECTOR] = (uint8_t)cur;
                b[(off + 1) % FATRO_SECTOR] = (uint8_t)(cur >> 8);
            } else if (type == 16) {
                w16(at(base + cl * 2 / FATRO_SECTOR) + cl * 2 % FATRO_SECTOR, val);
            } else {
                w32(at(base + cl * 4 / FATRO_SECTOR) + cl * 4 % FATRO_SECTOR, val);
            }
        }
    }
    uint32_t eoc() const { return type == 12 ? 0xFFF : type == 16 ? 0xFFFF : 0x0FFFFFFF; }

    static void dirent(uint8_t *e, const char *n83, uint8_t attr, uint32_t cl, uint32_t size) {
        memset(e, 0, 32);
        memcpy(e, n83, 11);
        e[11] = attr;
        w16(e + 20, cl >> 16);
        w16(e + 26, cl & 0xffff);
        w32(e + 28, size);
        w16(e + 22, 0x6000); w16(e + 24, 0x5CE4);
    }
    static uint8_t sum83(const char *n) {
        uint8_t sum = 0;
        for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)n[i]);
        return sum;
    }
    // Long-name entries (reverse order, last flagged 0x40) then the 8.3 entry.
    static uint32_t add_lfn(uint8_t *dir, uint32_t slot, const char *n83, const char *lname) {
        static const uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        uint32_t len = (uint32_t)strlen(lname), nent = (len + 12) / 13;
        for (uint32_t seq = nent; seq >= 1; seq--) {
            uint8_t *e = dir + 32 * slot++;
            memset(e, 0, 32);
            e[0] = (uint8_t)(seq | (seq == nent ? 0x40 : 0));
            e[11] = 0x0F;
            e[13] = sum83(n83);
            bool pad = false;
            for (uint32_t i = 0; i < 13; i++) {
                uint32_t idx = (seq - 1) * 13 + i;
                uint16_t c = idx < len ? (uint16_t)(uint8_t)lname[idx] : (!pad ? (pad = true, 0x0000) : 0xFFFF);
                w16(e + off[i], c);
            }
        }
        return slot;
    }

    void build(int t) {
        type = t;
        if (t == 12) { total = 64; fatsz = 1; rsvd = 1; root_ents = 16; spc = 1; }
        if (t == 16) { total = 5000; fatsz = 20; rsvd = 1; root_ents = 16; spc = 1; }
        if (t == 32) { total = 70000; fatsz = 547; rsvd = 32; root_ents = 0; spc = 1; root_cluster = 2; }
        first_fat = rsvd;
        uint32_t root_secs = (root_ents * 32 + 511) / 512;
        first_root = first_fat + nfats * fatsz;
        first_data = first_root + root_secs;

        uint8_t *b = at(0);
        b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90; memcpy(b + 3, "FUZZVOL ", 8);
        w16(b + 11, 512); b[13] = (uint8_t)spc; w16(b + 14, rsvd); b[16] = (uint8_t)nfats;
        w16(b + 17, root_ents);
        if (total < 65536) w16(b + 19, total); else w32(b + 32, total);
        b[21] = 0xF8;
        w16(b + 24, 32); w16(b + 26, 2);
        if (t == 32) {
            w32(b + 36, fatsz); w32(b + 44, root_cluster); w16(b + 48, 1); w16(b + 50, 6);
            memcpy(b + 71, "FUZZVOL    ", 11); memcpy(b + 82, "FAT32   ", 8);
        } else {
            w16(b + 22, fatsz);
            memcpy(b + 43, "FUZZVOL    ", 11); memcpy(b + 54, t == 12 ? "FAT12   " : "FAT16   ", 8);
        }
        b[t == 32 ? 66 : 38] = 0x29;                         // the extended boot signature
        w16(b + 510, 0xAA55);
        if (t == 32) {                                       // FSInfo
            uint8_t *fi = at(1);
            w32(fi, 0x41615252); w32(fi + 484, 0x61417272); w32(fi + 488, 60000); w32(fi + 492, 9);
            w32(fi + 508, 0xAA550000);
        }

        fat_set(0, t == 12 ? 0xFF8 : t == 16 ? 0xFFF8 : 0x0FFFFFF8);
        fat_set(1, eoc());
        uint32_t c0 = (t == 32) ? 3 : 2;                     // FAT32's root owns cluster 2
        if (t == 32) fat_set(2, eoc());
        fat_set(c0, eoc());                                  // HELLO.TXT
        fat_set(c0 + 1, c0 + 2); fat_set(c0 + 2, eoc());     // DATA.BIN, two clusters
        fat_set(c0 + 3, eoc());                              // SUB
        fat_set(c0 + 4, eoc());                              // SUB/INNER.TXT

        uint8_t *root = at(t == 32 ? lba_of(2) : first_root);
        uint32_t slot = 0;
        Builder::dirent(root + 32 * slot++, "FUZZVOL    ", 0x08, 0, 0);
        slot = add_lfn(root, slot, "LONGNA~1TXT", "A rather long file name.txt");
        Builder::dirent(root + 32 * slot++, "LONGNA~1TXT", 0x20, c0, 12);
        Builder::dirent(root + 32 * slot++, "DATA    BIN", 0x20, c0 + 1, 600);
        Builder::dirent(root + 32 * slot++, "SUB        ", 0x10, c0 + 3, 0);
        Builder::dirent(root + 32 * slot++, "HELLO   TXT", 0x20, c0, 12);

        memcpy(at(lba_of(c0)), "hello world\n", 12);
        memset(at(lba_of(c0 + 1)), 'D', 512);
        memset(at(lba_of(c0 + 2)), 'E', 88);
        uint8_t *sub = at(lba_of(c0 + 3));
        Builder::dirent(sub + 0,  ".          ", 0x10, c0 + 3, 0);
        Builder::dirent(sub + 32, "..         ", 0x10, 0, 0);
        Builder::dirent(sub + 64, "INNER   TXT", 0x20, c0 + 4, 5);
        memcpy(at(lba_of(c0 + 4)), "inner", 5);
    }
};

struct Seen { uint32_t n; bool ok; };
void list_cb(void *ctx, const FatRoEntry *e) {
    Seen *s = (Seen *)ctx;
    s->n++;
    if (!memchr(e->name, 0, sizeof(e->name))) s->ok = false;
}

}  // namespace

static void fuzz_fat(uint32_t iters) {
    static Builder seeds[3];
    const int kinds[3] = {12, 16, 32};
    for (int i = 0; i < 3; i++) {
        seeds[i].build(kinds[i]);
        // The seed itself must be a volume this reader accepts, or the fuzzing proves nothing.
        Image im; im.seed = &seeds[i].s; im.budget = 100000;
        FatRoIo io{&im, img_read};
        static FatRo f;
        memset(&f, 0, sizeof(f));
        bool ok = fatro_mount(&f, &io);
        ck(ok && f.type == (kinds[i] == 12 ? FATRO_FAT12 : kinds[i] == 16 ? FATRO_FAT16 : FATRO_FAT32),
           "a seed volume does not mount as its own type", (uint32_t)kinds[i]);
        FatRoEntry e;
        uint8_t buf[16];
        ck(ok && fatro_stat(&f, "/SUB/INNER.TXT", &e) && e.size == 5 &&
           fatro_read(&f, &e, nullptr, 0, buf, sizeof(buf)) == 5 && !memcmp(buf, "inner", 5),
           "a seed volume's file does not read back", (uint32_t)kinds[i]);
        ck(ok && fatro_stat(&f, "/A rather long file name.txt", &e) && e.size == 12,
           "a seed volume's long name is not found", (uint32_t)kinds[i]);
        fatro_unmount(&f);
    }

    const char *paths[] = {"/", "/SUB", "/HELLO.TXT", "/SUB/INNER.TXT", "/DATA.BIN", "/NOPE",
                           "/SUB/..", "//", "/SUB/", "/sub/inner.txt", "/HELLO.TXT/x",
                           "/A rather long file name.txt"};
    static FatRo f;
    for (uint32_t it = 0; it < iters; it++) {
        const Builder &seed = seeds[it % 3];
        Image im;
        im.seed = &seed.s;
        // Damage a few bytes in a few of the sectors that hold something.
        std::vector<uint32_t> held;
        for (auto &kv : seed.s) held.push_back(kv.first);
        uint32_t hits = 1 + below(6);
        for (uint32_t h = 0; h < hits; h++) {
            uint32_t lba = below(10) < 8 ? held[below((uint32_t)held.size())] : below(seed.total);
            auto cur = im.over.find(lba);
            Sec sec;
            if (cur != im.over.end()) sec = cur->second;
            else { auto sd = seed.s.find(lba); if (sd != seed.s.end()) sec = sd->second; else sec.fill(0); }
            Bytes one(sec.begin(), sec.end());
            mutate(one, 512);
            one.resize(512, 0);
            memcpy(sec.data(), one.data(), 512);
            im.over[lba] = sec;
        }
        im.budget = 20000;
        FatRoIo io{&im, img_read};
        memset(&f, 0, sizeof(f));
        if (!fatro_mount(&f, &io)) continue;
        im.geom = &f;

        (void)fatro_type_name(&f);
        (void)fatro_total_bytes(&f);
        (void)fatro_free_bytes(&f);
        for (const char *p : paths) {
            Seen s{0, true};
            fatro_list(&f, p, list_cb, &s);
            ck(s.ok, "a listed name was not terminated", it);
            FatRoEntry e;
            if (fatro_stat(&f, p, &e) && !e.is_dir) {
                ck(memchr(e.name, 0, sizeof(e.name)) != nullptr, "a stat name was not terminated", it);
                FatRoCursor cur;
                fatro_cursor_init(&cur);
                uint8_t buf[200];
                uint64_t got = 0;
                for (uint32_t off = 0; off < e.size && off < 4096; ) {
                    uint32_t n = fatro_read(&f, &e, &cur, off, buf, sizeof(buf));
                    ck(n <= sizeof(buf), "a read returned more than it was asked for", it);
                    if (!n) break;
                    off += n; got += n;
                }
                ck(got <= e.size, "a file read past its recorded size", it);
                // And a seek a very long way in, which walks the chain a cluster at a time.
                FatRoCursor far;
                fatro_cursor_init(&far);
                (void)fatro_read(&f, &e, &far, e.size ? e.size - 1 : 0, buf, sizeof(buf));
                (void)fatro_read(&f, &e, nullptr, 0x7ffff000u, buf, sizeof(buf));
            }
        }
        fatro_unmount(&f);
        ck(!im.over_budget, "a volume sent the reader round a loop (read budget exhausted)", it);
        ck(!im.escaped, "the reader read outside the volume its boot sector describes", it);
    }
}

// --- small parsers ----------------------------------------------------------------------------

static void fuzz_small(uint32_t iters) {
    std::vector<Bytes> corpus = {
        S("ls -l /pkg"), S("echo \"hello world\" tail"), S("curl -d '{\"k\":1}' -H 'A: b' http://x"),
        S("a && b || c ; d | e > f"), S("cat \"unterminated"), S("x 'y"), S("   "), S(""),
        S("echo don't; ls >> 'out file'"),
    };
    for (uint32_t it = 0; it < iters; it++) {
        Bytes in = pick(corpus);
        mutate(in, 300);
        in.push_back(0);
        char line[512];
        size_t n = in.size() < sizeof(line) ? in.size() : sizeof(line) - 1;
        memcpy(line, in.data(), n); line[n] = 0;

        char copy[512];
        memcpy(copy, line, sizeof(copy));
        char *argv[16];
        int argc = cmdline_split_args(copy, argv, 16);
        ck(argc >= 0 && argc <= 16, "argc out of range", it);
        for (int i = 0; i < argc; i++)
            ck(argv[i] >= copy && argv[i] < copy + sizeof(copy), "argv points outside the line", it);

        memcpy(copy, line, sizeof(copy));
        Connector k; int skip = 0;
        char *p = cmdline_next_connector(copy, &k, &skip);
        ck(!p || (p >= copy && p < copy + strlen(copy) + 1 && (skip == 1 || skip == 2)), "connector out of range", it);
        memcpy(copy, line, sizeof(copy));
        p = cmdline_next_pipe(copy);
        ck(!p || (p >= copy && p < copy + strlen(copy)), "pipe out of range", it);
        memcpy(copy, line, sizeof(copy));
        bool app;
        char *file = cmdline_split_redirect(copy, &app);
        ck(!file || (file >= copy && file < copy + sizeof(copy)), "redirect file out of range", it);
        memcpy(copy, line, sizeof(copy));
        char *t = cmdline_trim(copy);
        ck(t >= copy && t <= copy + strlen(copy), "trim out of range", it);

        unsigned mhz;
        clock_parse_mhz(line, &mhz);

        // Escape decoding: the bytes after ESC, whatever they are.
        line_decode_escape(line, (uint32_t)(strlen(line) < 16 ? strlen(line) : 16));
    }
}

// A parser that can be sent round a loop forever must fail the run, not stall it.
static const char *g_target = "";
static void on_alarm(int) {
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "  FAIL: the %s target stopped making progress (hang)\n", g_target);
    if (n > 0) { ssize_t r = write(2, msg, (size_t)n); (void)r; }
    _exit(2);
}

int main(void) {
    signal(SIGALRM, on_alarm);
    const char *al = getenv("RPC_FUZZ_ALARM");
    unsigned alarm_s = al ? (unsigned)strtoul(al, nullptr, 10) : 240;
    const char *e = getenv("RPC_FUZZ_ITERS");
    uint32_t iters = e ? (uint32_t)strtoul(e, nullptr, 10) : 20000;
    const char *s = getenv("RPC_FUZZ_SEED");
    g_rng = 0x9E3779B97F4A7C15ull ^ (s ? strtoull(s, nullptr, 10) * 0x100000001B3ull : 1);
    if (!g_rng) g_rng = 1;

    struct { const char *name; void (*fn)(uint32_t); uint32_t share; } targets[] = {
        {"http",     fuzz_http,     1},
        {"url",      fuzz_url,      1},
        {"repo",     fuzz_repo,     1},
        {"pkgindex", fuzz_pkgindex, 1},
        {"fat",      fuzz_fat,      1},
        {"small",    fuzz_small,    1},
    };
    for (auto &t : targets) {
        int before = fails;
        g_target = t.name;
        alarm(alarm_s);
        t.fn(iters * t.share);
        alarm(0);
        printf("  fuzz %-9s %u inputs%s\n", t.name, iters * t.share, fails == before ? "" : "  FAILED");
    }
    printf("  fuzz: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
