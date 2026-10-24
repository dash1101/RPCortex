// Mutation fuzzing of the package loader.
//
// The loader reads an ELF relocatable object that somebody else produced: a
// package fetched from a repository, or copied on with `put`. It trusts nothing
// in it that it has not checked — section offsets and sizes, the symbol table,
// relocation entries, string indices — and a mistake in any of them is a write
// to the wrong place in a device with no memory protection around the loader
// itself.
//
// The seeds are the packages build.sh produces, so the mutations start from real
// compiler output rather than from what a test author thought to put in an
// object. Damage is aimed at the structures the loader walks (the section header
// table, symbol and string tables, relocation sections, the app header) about as
// often as at the file at large, because a random byte flip mostly breaks the
// ELF magic and is refused on the first line.
//
// Each allocation the loader makes is its own mapping with a guard page on both
// sides, so an overrun is a fault with a stack and not a quiet write into
// whatever the allocator put next door. After every input all of them must have
// been given back, on the failure paths as much as on success: a load that fails
// and leaks is a device that runs out of memory after enough bad packages.
//
//     RPC_FUZZ_ITERS=200000 RPC_FUZZ_SEED=3 ./fuzz_loader_test

#include "loader.h"
#include "elf.h"
#include "pkgslot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>
#include <map>
#include <string>

typedef std::vector<uint8_t> Bytes;

static int checks = 0, fails = 0;
static void ck(bool c, const char *what, uint32_t iter) {
    checks++;
    if (!c) { fails++; if (fails < 20) printf("  FAIL: %s  (iteration %u)\n", what, iter); }
}

// --- the generator ---------------------------------------------------------------

static uint64_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

// --- memory the loader can be caught overrunning ----------------------------------------
//
// The loader keeps addresses in uint32_t, so every block must live below 4 GB
// (MAP_32BIT), and a branch instruction reaches 16 MB, so the code, the data and
// the veneer pool must be near each other: separate mappings scatter across a
// window much wider than that, and the loader then fails calls that no device
// would. So ONE region is reserved up front and blocks are carved out of it, each
// with an inaccessible page on both sides and placed to END at the page after it,
// so that an overrun by one byte faults. Alignment of 16 is kept (the loader asks
// for multiples of 8 and aligns inside what it got).
//
// The region starts again from the beginning whenever nothing is live, which is
// every input, and a freed block is made inaccessible, so a use after free faults
// as well.

static const size_t kPage = 4096;
static const size_t kRegion = 8u << 20;
struct Block { size_t at, pages; };
static uint8_t *g_region;
static size_t g_used;
static std::map<void *, Block> g_live;
static size_t g_cap_bytes;              // a limit a run can set to starve the loader
static size_t g_live_bytes;

static void *guarded_alloc(size_t n) {
    if (g_cap_bytes && g_live_bytes + n > g_cap_bytes) return nullptr;
    if (!g_region) {
        void *m = mmap(nullptr, kRegion, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (m == MAP_FAILED) return nullptr;
        g_region = (uint8_t *)m;
    }
    size_t body = (n + 15) & ~(size_t)15;
    size_t pages = (body + kPage - 1) / kPage;
    size_t need = (pages + 2) * kPage;
    if (g_used + need > kRegion) return nullptr;
    uint8_t *base = g_region + g_used;
    if (mprotect(base + kPage, pages * kPage, PROT_READ | PROT_WRITE) != 0) return nullptr;
    uint8_t *p = base + kPage + pages * kPage - body;
    memset(p, 0xA5, body);              // uninitialised reads show as a pattern, not as luck
    g_live[p] = Block{g_used, pages};
    g_used += need;
    g_live_bytes += body;
    return p;
}
static void guarded_free(void *p) {
    if (!p) return;
    auto it = g_live.find(p);
    if (it == g_live.end()) {
        fprintf(stderr, "  FAIL: the loader freed a pointer it was never given (%p)\n", p);
        abort();
    }
    uint8_t *body = g_region + it->second.at + kPage;
    mprotect(body, it->second.pages * kPage, PROT_NONE);
    madvise(body, it->second.pages * kPage, MADV_DONTNEED);
    g_live.erase(it);
    if (g_live.empty()) { g_used = 0; g_live_bytes = 0; }
}

// --- the firmware's symbol table, faked as in realapp_test ----------------------------------

#define FAKE_SYMS 256
static char g_seen[FAKE_SYMS][40];
static uint32_t g_addr[FAKE_SYMS];
static int g_ns;
static int api_slot(const char *n) {
    for (int i = 0; i < g_ns; i++) if (!strcmp(g_seen[i], n)) return i;
    if (g_ns >= FAKE_SYMS) return -1;
    snprintf(g_seen[g_ns], 40, "%s", n);
    g_addr[g_ns] = (0x10001000u + (uint32_t)g_ns * 4u) | 1u;
    return g_ns++;
}
uint32_t api_lookup(const char *n) { int i = api_slot(n); return i < 0 ? 0 : g_addr[i]; }
int api_index_of(const char *n) { return api_slot(n); }
uint32_t api_addr_at(uint32_t i) { return i < (uint32_t)g_ns ? g_addr[i] : 0; }
uint32_t api_symbol_count(void) { return FAKE_SYMS; }
uint32_t api_abi_prefix_crc(uint32_t count) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < count; i++) crc = pkgslot_crc32(crc, &i, sizeof(i));
    return crc;
}

// --- mutation -------------------------------------------------------------------------------------

static const uint32_t kBoundary[] = {0, 1, 2, 3, 4, 8, 0x7f, 0x80, 0xff, 0x100, 0xffff, 0x10000,
                                     0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffff0u, 0x40000000u};

struct Region { uint32_t off, len; };

// Where the structures the loader walks live in this seed.
static std::vector<Region> regions_of(const Bytes &b) {
    std::vector<Region> out;
    if (b.size() < sizeof(Elf32_Ehdr)) return out;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)b.data();
    out.push_back({0, (uint32_t)sizeof(Elf32_Ehdr)});
    if ((uint64_t)eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf32_Shdr) > b.size()) return out;
    out.push_back({eh->e_shoff, (uint32_t)(eh->e_shnum * sizeof(Elf32_Shdr))});
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(b.data() + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        uint32_t t = sh[i].sh_type;
        if ((t == SHT_REL || t == SHT_RELA || t == SHT_SYMTAB || t == SHT_STRTAB ||
             (t == SHT_PROGBITS && sh[i].sh_size <= 64)) &&
            (uint64_t)sh[i].sh_offset + sh[i].sh_size <= b.size() && sh[i].sh_size)
            out.push_back({sh[i].sh_offset, sh[i].sh_size});
    }
    return out;
}

static void put32(Bytes &b, uint32_t at, uint32_t v) {
    for (int i = 0; i < 4 && at + i < b.size(); i++) b[at + i] = (uint8_t)(v >> (8 * i));
}

static void mutate_image(Bytes &b, const std::vector<Region> &regs) {
    uint32_t ops = 1 + below(3);
    while (ops--) {
        uint32_t n = (uint32_t)b.size();
        if (n < 8) return;
        uint32_t at;
        if (!regs.empty() && below(10) < 6) {
            const Region &r = regs[below((uint32_t)regs.size())];
            at = r.off + below(r.len ? r.len : 1);
        } else {
            at = below(n);
        }
        if (at >= n) at = n - 1;
        switch (below(7)) {
        case 0: b[at] ^= (uint8_t)(1u << below(8)); break;
        case 1: b[at] = (uint8_t)rnd(); break;
        case 2: put32(b, at & ~3u, kBoundary[below(sizeof(kBoundary) / sizeof(kBoundary[0]))]); break;
        case 3: put32(b, at & ~3u, rnd()); break;
        case 4: {                                           // a small step either way: an off-by-one
            uint32_t w = 0;
            uint32_t a4 = at & ~3u;
            for (int i = 0; i < 4 && a4 + i < n; i++) w |= (uint32_t)b[a4 + i] << (8 * i);
            put32(b, a4, w + (below(2) ? 1u : (uint32_t)-1));
            break;
        }
        case 5: if (n > 64) b.resize(n - 1 - below(n / 4 < 200 ? n / 4 : 200)); break;   // truncate
        default: {                                          // copy a word from elsewhere
            uint32_t src = below(n) & ~3u, a4 = at & ~3u;
            for (int i = 0; i < 4 && a4 + i < n && src + i < n; i++) b[a4 + i] = b[src + i];
            break;
        }
        }
    }
}

// --- driving the loader --------------------------------------------------------------------------

struct Src { const uint8_t *p; uint32_t n; bool fail_late; uint32_t reads; };
static int src_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    Src *s = (Src *)ctx;
    s->reads++;
    if (s->fail_late && s->reads > 12) return -1;           // a card that goes away part way through
    if (off > s->n) return -1;
    uint32_t n = len;
    if (n > s->n - off) n = s->n - off;
    memcpy(dst, s->p + off, n);
    return (int)n;
}

struct SlotBuf { uint8_t *buf; uint32_t cap; };
static bool slot_sink(void *ctx, uint32_t off, const void *data, uint32_t len) {
    SlotBuf *s = (SlotBuf *)ctx;
    if ((uint64_t)off + len > s->cap) return false;
    memcpy(s->buf + off, data, len);
    return true;
}

static bool in_range(LoadResult r) { return r >= LOAD_OK && r <= LOAD_ERR_BAD_SECTION; }

static Bytes read_file(const std::string &path) {
    Bytes b;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return b;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return b;
}

static const char *kDirs[] = {"../build_pico2_w/apps", "../build_pico_w/apps", "../build_pico2/apps",
                              "../build_pico/apps", "../build/apps"};
static const char *kNames[] = {"greet", "bench", "calc", "dht", "gpio", "tuidemo", "backup"};

static void one(const Bytes &img, uint32_t it, bool svc, bool starve) {
    Src s{img.data(), (uint32_t)img.size(), (it % 13) == 0, 0};
    AppSource src{&s, src_read, (uint32_t)img.size()};
    g_cap_bytes = starve ? 4096 + below(40000) : 0;
    loader_set_veneer_mode(svc ? LOADER_VENEER_SVC : LOADER_VENEER_DIRECT);

    RpcAppHeader h;
    memset(&h, 0, sizeof(h));
    s.reads = 0;
    LoadResult r = app_peek(src, &h);
    ck(in_range(r), "app_peek returned a result outside the enum", it);

    s.reads = 0;
    LoadedApp app;
    memset(&app, 0, sizeof(app));
    r = app_load(src, &app);
    ck(in_range(r), "app_load returned a result outside the enum", it);
    if (r == LOAD_OK) {
        ck(app.image != nullptr || app.text_size == 0, "a loaded app has no image", it);
        ck(app.entry != nullptr, "a loaded app has no entry point", it);
        ck(app.bytes_allocated < (1u << 24), "a loaded app claims an absurd footprint", it);
        ck(memchr(app.header.name, 0, sizeof(app.header.name)) != nullptr, "app name not terminated", it);
        app_unload(&app);
    }
    ck(memchr(app.detail, 0, sizeof(app.detail)) != nullptr, "detail text not terminated", it);

    s.reads = 0;
    PicMeasure pm;
    memset(&pm, 0, sizeof(pm));
    r = app_pic_measure(src, &pm);
    ck(in_range(r), "app_pic_measure returned a result outside the enum", it);

    // The slot path, only meaningful in SVC mode.
    if (svc) {
        SlotBuf sb;
        sb.cap = 96 * 1024;
        sb.buf = (uint8_t *)guarded_alloc(sb.cap);
        if (sb.buf) {
            s.reads = 0;
            PicManifest m;
            memset(&m, 0, sizeof(m));
            r = app_pic_install(src, slot_sink, &sb, &m);
            ck(in_range(r), "app_pic_install returned a result outside the enum", it);
            if (r == LOAD_OK) {
                LoadedApp pa;
                memset(&pa, 0, sizeof(pa));
                LoadResult lr = app_pic_load(sb.buf, &m, &pa);
                ck(in_range(lr), "app_pic_load returned a result outside the enum", it);
                if (lr == LOAD_OK) app_unload(&pa);
                app_pic_manifest_free(&m);
            }
            guarded_free(sb.buf);
        }
    }
    g_cap_bytes = 0;
    ck(g_live.empty(), "the loader kept memory after the package was unloaded (a leak on some path)", it);
    if (!g_live.empty()) {                                  // report once, then clean up so one leak is one failure
        for (auto &kv : std::map<void *, Block>(g_live)) guarded_free(kv.first);
    }
}


// --- the edges, exactly ----------------------------------------------------------------------------
//
// Random damage almost never lands on a value that is wrong by one, and those are
// the ones that matter: a relocation whose site ends one byte past its section, a
// name table with no final NUL, an alignment that is not a power of two. Each is
// built here on purpose from a real package and judged against the answer it must
// give — refused where it is wrong and, just as important, ACCEPTED where it is
// exactly right, so a check cannot get away with being too strict.

struct ElfView {
    Bytes b;
    Elf32_Ehdr *eh() { return (Elf32_Ehdr *)b.data(); }
    Elf32_Shdr *sh() { return (Elf32_Shdr *)(b.data() + eh()->e_shoff); }
    // The first relocation of `type` whose target is writable (or not).
    Elf32_Rel *find_rel(uint32_t type, bool writable, uint32_t *target_size) {
        for (int i = 0; i < eh()->e_shnum; i++) {
            if (sh()[i].sh_type != SHT_REL) continue;
            uint32_t t = sh()[i].sh_info;
            if (t >= eh()->e_shnum || !(sh()[t].sh_flags & SHF_ALLOC)) continue;
            if (((sh()[t].sh_flags & SHF_WRITE) != 0) != writable) continue;
            uint32_t n = sh()[i].sh_size / sizeof(Elf32_Rel);
            for (uint32_t r = 0; r < n; r++) {
                Elf32_Rel *rel = (Elf32_Rel *)(b.data() + sh()[i].sh_offset) + r;
                if (ELF32_R_TYPE(rel->r_info) == type) { *target_size = sh()[t].sh_size; return rel; }
            }
        }
        return nullptr;
    }
};

static LoadResult load_direct(const Bytes &img) {
    Src s{img.data(), (uint32_t)img.size(), false, 0};
    AppSource src{&s, src_read, (uint32_t)img.size()};
    loader_set_veneer_mode(LOADER_VENEER_DIRECT);
    LoadedApp a; memset(&a, 0, sizeof(a));
    LoadResult r = app_load(src, &a);
    if (r == LOAD_OK) app_unload(&a);
    ck(g_live.empty(), "a boundary case left memory behind", 0);
    for (auto &kv : std::map<void *, Block>(g_live)) guarded_free(kv.first);
    return r;
}

static LoadResult install_slot(const Bytes &img, bool also_load) {
    Src s{img.data(), (uint32_t)img.size(), false, 0};
    AppSource src{&s, src_read, (uint32_t)img.size()};
    loader_set_veneer_mode(LOADER_VENEER_SVC);
    SlotBuf sb{nullptr, 160 * 1024};
    sb.buf = (uint8_t *)guarded_alloc(sb.cap);
    PicManifest m; memset(&m, 0, sizeof(m));
    LoadResult r = app_pic_install(src, slot_sink, &sb, &m);
    if (r == LOAD_OK) {
        if (also_load) {
            LoadedApp pa; memset(&pa, 0, sizeof(pa));
            if (app_pic_load(sb.buf, &m, &pa) == LOAD_OK) app_unload(&pa);
        }
        app_pic_manifest_free(&m);
    }
    guarded_free(sb.buf);
    ck(g_live.empty(), "a boundary case left memory behind", 1);
    for (auto &kv : std::map<void *, Block>(g_live)) guarded_free(kv.first);
    return r;
}

static void boundary_cases(const std::vector<Bytes> &seeds, const Bytes &novad1) {
    const uint32_t kOk = 0xFFFFFFFFu;          // "any result but a range refusal"
    for (size_t k = 0; k < seeds.size(); k++) {
        // Word relocations (no alignment) and Thumb branches (halfword aligned).
        struct { uint32_t type; bool even; const char *name; } kinds[] = {
            {R_ARM_ABS32, false, "ABS32"}, {R_ARM_THM_CALL, true, "THM_CALL"}};
        for (auto &kd : kinds) {
            ElfView v; v.b = seeds[k];
            uint32_t tsz = 0;
            if (!v.find_rel(kd.type, false, &tsz) || tsz < 8) continue;
            const struct { uint32_t off; bool good; } cases[] = {
                {tsz - 4, true}, {tsz - 3, false}, {tsz - 2, false}, {tsz - 1, false},
                {tsz, false}, {tsz + 4, false}, {0xFFFFFFFCu, false}, {0xFFFFFFFEu, false}, {0x80000000u, false}};
            for (auto &c : cases) {
                ElfView w; w.b = seeds[k];
                uint32_t sz2;
                Elf32_Rel *rel = w.find_rel(kd.type, false, &sz2);
                rel->r_offset = c.off;
                LoadResult r = load_direct(w.b);
                bool refused = (r == LOAD_ERR_RELOC_RANGE);
                // An odd offset is only wrong for a Thumb instruction.
                bool expect_good = c.good && (kd.even ? (c.off % 2 == 0) : true);
                if (!kd.even && c.off == tsz - 3) expect_good = false;   // 4 bytes from here pass the end
                (void)kOk;
                ck(expect_good ? !refused : refused, kd.name, (uint32_t)(k * 100 + (c.off & 0xff)));
            }
        }

        // A name table that does not end in a NUL: the last string runs off it.
        {
            ElfView v; v.b = seeds[k];
            Elf32_Shdr &ss = v.sh()[v.eh()->e_shstrndx];
            if (ss.sh_size && ss.sh_offset + ss.sh_size <= v.b.size()) {
                v.b[ss.sh_offset + ss.sh_size - 1] = 'Z';
                (void)load_direct(v.b);                            // must not read past the table
                Src s{v.b.data(), (uint32_t)v.b.size(), false, 0};
                AppSource src{&s, src_read, (uint32_t)v.b.size()};
                RpcAppHeader h; (void)app_peek(src, &h);
                PicMeasure pm; (void)app_pic_measure(src, &pm);
                (void)install_slot(v.b, false);
            }
        }

        // The same, aimed: the section that IS the app header is named by the last
        // thing in the table, with no NUL after it. The reader supplies the
        // terminator, so this package is exactly what it says and must load; a
        // reader that trusted the file would compare the name against whatever
        // followed the buffer and not find the header at all.
        {
            ElfView v; v.b = seeds[k];
            Elf32_Shdr &ss = v.sh()[v.eh()->e_shstrndx];
            static const char kName[] = ".rpc_app_header";
            const uint32_t n = (uint32_t)sizeof(kName) - 1;
            int hi = -1;
            if (ss.sh_size > n + 1 && ss.sh_offset + ss.sh_size <= v.b.size())
                for (int i = 0; i < v.eh()->e_shnum; i++)
                    if (v.sh()[i].sh_name < ss.sh_size &&
                        !strcmp((const char *)&v.b[ss.sh_offset + v.sh()[i].sh_name], kName)) { hi = i; break; }
            if (hi >= 0) {
                memcpy(&v.b[ss.sh_offset + ss.sh_size - n], kName, n);
                v.sh()[hi].sh_name = ss.sh_size - n;
                ck(load_direct(v.b) == LOAD_OK, "a name at the very end of the table, unterminated by the file (load)", (uint32_t)k);
                Src sp{v.b.data(), (uint32_t)v.b.size(), false, 0};
                AppSource srcp{&sp, src_read, (uint32_t)v.b.size()};
                RpcAppHeader hp;
                ck(app_peek(srcp, &hp) == LOAD_OK, "a name at the very end of the table, unterminated by the file (peek)", (uint32_t)k);
                ck(install_slot(v.b, false) != LOAD_ERR_NO_HEADER, "a name at the very end of the table, unterminated by the file (slot)", (uint32_t)k);
            }
        }

        // A section alignment: a power of two up to a page is a layout, anything
        // else is not.
        for (uint32_t al : {8u, 16u, 64u, 4096u, 12u, 24u, 0x300u, 8192u, 0x80000000u}) {
            ElfView v; v.b = seeds[k];
            int idx = -1;
            for (int i = 0; i < v.eh()->e_shnum; i++)
                if ((v.sh()[i].sh_flags & SHF_ALLOC) && v.sh()[i].sh_size) { idx = i; break; }
            if (idx < 0) continue;
            v.sh()[idx].sh_addralign = al;
            LoadResult r = load_direct(v.b);
            bool pow2 = al <= 4096 && (al & (al - 1)) == 0;
            ck(pow2 ? r == LOAD_OK : r == LOAD_ERR_BAD_SECTION, "section alignment", (uint32_t)(k * 100000 + al));
        }

        // A section too large to lay out, however it is written down.
        for (uint32_t sz : {0xFFFFFF00u, 0x7FFFFFFFu, 0x01000001u}) {
            ElfView v; v.b = seeds[k];
            int idx = -1;
            for (int i = 0; i < v.eh()->e_shnum; i++)
                if ((v.sh()[i].sh_flags & SHF_ALLOC) && v.sh()[i].sh_size && v.sh()[i].sh_type == SHT_NOBITS) { idx = i; break; }
            if (idx < 0) continue;
            v.sh()[idx].sh_size = sz;
            ck(load_direct(v.b) == LOAD_ERR_BAD_SECTION, "a section that cannot be laid out", (uint32_t)(k * 100 + (sz & 0xff)));
        }
    }

    // The writable-data fixups, which exist in only one real package. They are
    // recorded at install and applied by app_pic_load with no second check, so the
    // check at install is the whole defence.
    if (!novad1.empty()) {
        ElfView v; v.b = novad1;
        uint32_t tsz = 0;
        if (v.find_rel(R_ARM_ABS32, true, &tsz) && tsz >= 8) {
            const struct { uint32_t off; bool good; } cases[] = {
                {tsz - 4, true}, {tsz - 3, false}, {tsz, false}, {tsz + 4, false},
                {0x1000, false}, {0xFFFFFFFCu, false}};
            for (auto &c : cases) {
                ElfView w; w.b = novad1;
                uint32_t z;
                Elf32_Rel *rel = w.find_rel(R_ARM_ABS32, true, &z);
                if (c.off < z - 3 + 0 && c.off != tsz - 4) continue;     // keep to the edges
                rel->r_offset = c.off;
                LoadResult r = install_slot(w.b, true);
                bool refused = (r == LOAD_ERR_RELOC_RANGE);
                ck(c.good ? !refused : refused, "writable-data fixup site", c.off);
            }
        } else {
            ck(false, "novad1 has no writable relocation to test (the seed changed?)", 0);
        }
    }
}

static const char *g_stage = "";
static uint32_t g_iter;
static void on_alarm(int) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "  FAIL: the loader stopped making progress on %s (hang, iteration %u)\n", g_stage, g_iter);
    if (n > 0) { ssize_t r = write(2, msg, (size_t)n); (void)r; }
    _exit(2);
}

int main(void) {
    signal(SIGALRM, on_alarm);
    const char *e = getenv("RPC_FUZZ_ITERS");
    uint32_t iters = e ? (uint32_t)strtoul(e, nullptr, 10) : 4000;
    const char *s = getenv("RPC_FUZZ_SEED");
    g_rng = 0x9E3779B97F4A7C15ull ^ (s ? strtoull(s, nullptr, 10) * 0x100000001B3ull : 1);
    if (!g_rng) g_rng = 1;
    const char *al = getenv("RPC_FUZZ_ALARM");
    unsigned alarm_s = al ? (unsigned)strtoul(al, nullptr, 10) : 240;

    loader_set_allocator(guarded_alloc, guarded_free);

    // The seeds: the packages build.sh produced.
    std::vector<Bytes> seeds;
    std::string dir;
    for (const char *d : kDirs) {
        int have = 0;
        for (const char *n : kNames) if (!read_file(std::string(d) + "/" + n + ".app").empty()) have++;
        if (have == (int)(sizeof(kNames) / sizeof(kNames[0]))) { dir = d; break; }
    }
    if (dir.empty()) {
        printf("  FAIL: no built packages to use as seeds.\n"
               "        Run ./build.sh from the repository root (or ./build.sh pico2_w) first.\n");
        return 1;
    }
    for (const char *n : kNames) seeds.push_back(read_file(dir + "/" + n + ".app"));
    printf("  from %s, %zu seeds\n", dir.c_str(), seeds.size());

    // Every seed must load as it is, in both modes, or the damage proves nothing.
    for (size_t i = 0; i < seeds.size(); i++) {
        Src sc{seeds[i].data(), (uint32_t)seeds[i].size(), false, 0};
        AppSource src{&sc, src_read, (uint32_t)seeds[i].size()};
        loader_set_veneer_mode(LOADER_VENEER_DIRECT);
        LoadedApp a; memset(&a, 0, sizeof(a));
        LoadResult r = app_load(src, &a);
        if (r != LOAD_OK)
            printf("  seed %s: %s (%s)\n", kNames[i], load_result_str(r), a.detail);
        ck(r == LOAD_OK, "an unmodified seed package does not load", (uint32_t)i);
        if (r == LOAD_OK) app_unload(&a);
        ck(g_live.empty(), "an unmodified seed leaks", (uint32_t)i);
    }

    Bytes novad1 = read_file(dir + "/novad1.app");
    boundary_cases(seeds, novad1);
    printf("  fuzz loader      boundary cases\n");

    std::vector<std::vector<Region>> regs;
    for (auto &b : seeds) regs.push_back(regions_of(b));

    g_stage = "mutated packages";
    alarm(alarm_s);
    for (uint32_t it = 0; it < iters; it++) {
        g_iter = it;
        size_t k = below((uint32_t)seeds.size());
        Bytes img = seeds[k];
        mutate_image(img, regs[k]);
        one(img, it, (it & 1) != 0, (it % 7) == 3);
    }
    alarm(0);

    printf("  fuzz loader      %u inputs\n", iters);
    printf("  fuzz loader: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
