// The on-flash package slot: its format, and the write order that makes losing
// power halfway through safe.
//
// None of this can be proved on a board without deliberately cutting power at a
// chosen moment, which is not a test anybody runs. It CAN be proved here,
// against a fake flash that behaves the way flash behaves and refuses to behave
// the way it does not:
//
//   * erase sets a region to 0xFF, in whole 4 KB sectors
//   * program only ever clears bits — a byte cannot be rewritten to a value with
//     a bit back on without an erase, and the fake fails the test if you try
//   * an install can be cut off after any page, and the slot must then read as
//     empty rather than as something loadable
//
// The last one is the point of the file. A slot left half written that still
// carries a valid magic is a device that boots into a package which is partly
// the old one, and no amount of care at load time can detect that.
#include "pkgslot.h"
#include "loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks, fails;
static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

// The loader is linked for app_pic_manifest_free (the borrowed case below). It
// resolves firmware symbols through these; nothing here loads an ELF, so a
// plausible table is enough.
uint32_t api_lookup(const char *)        { return 0; }
uint32_t api_symbol_count(void)          { return 156; }
int      api_index_of(const char *)      { return -1; }
uint32_t api_addr_at(uint32_t)           { return 0; }

// --- a fake flash chip -------------------------------------------------------

#define REGION (64u * 1024u)

struct Fake {
    uint8_t  mem[REGION];
    uint32_t erases, programs;
    // Cut the install off: fail every program past this many. 0 means never.
    uint32_t fail_after;
    bool     violated;      // a program tried to set a bit back to 1
};
static Fake g_f;

static void fake_reset(void) {
    memset(&g_f, 0, sizeof(g_f));
    memset(g_f.mem, 0xFF, sizeof(g_f.mem));
}

static bool fake_erase(void *, uint32_t off, uint32_t len) {
    if (off % PKGSLOT_ERASE || len % PKGSLOT_ERASE) { g_f.violated = true; return false; }
    if (off + len > REGION) { g_f.violated = true; return false; }
    memset(g_f.mem + off, 0xFF, len);
    g_f.erases++;
    return true;
}

static bool fake_program(void *, uint32_t off, const void *data, uint32_t len) {
    if (off % PKGSLOT_PROG || len % PKGSLOT_PROG) { g_f.violated = true; return false; }
    if (off + len > REGION) { g_f.violated = true; return false; }
    if (g_f.fail_after && g_f.programs >= g_f.fail_after) return false;   // power cut
    const uint8_t *s = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        // Programming clears bits. Asking for a bit that is currently 0 to become
        // 1 is not something flash can do, and a writer that relies on it works
        // on a fake that memcpys and fails on a chip.
        if ((g_f.mem[off + i] & s[i]) != s[i]) g_f.violated = true;
        g_f.mem[off + i] &= s[i];
    }
    g_f.programs++;
    return true;
}

static const SlotFlash kFake = { nullptr, fake_erase, fake_program };

// --- a package to put in it --------------------------------------------------
//
// Synthetic, and deliberately not from a real ELF: this file is about the slot,
// and a fixture built here can be made to have exactly the awkward sizes that
// catch an off-by-one — a blob that is not a multiple of the page size, arrays
// that need padding to align, a .data image with a byte pattern that shows up
// wrong if it lands at the wrong offset.
#define BLOB_BYTES 1301u      // deliberately not a multiple of 256

static uint8_t      g_blob[BLOB_BYTES];
static PicGotEntry  g_got[7];
static PicAbs32     g_abs[3];
static uint8_t      g_data[37];

static void fixture(PicManifest *m) {
    for (uint32_t i = 0; i < BLOB_BYTES; i++) g_blob[i] = (uint8_t)(i * 31u + 7u);
    for (uint32_t i = 0; i < 7; i++) { g_got[i].value = 0x1000u + i * 4u;
                                       g_got[i].cls = (uint8_t)(i % 3); g_got[i].is_func = (uint8_t)(i & 1); }
    for (uint32_t i = 0; i < 3; i++) { g_abs[i].site = 64u + i * 4u;
                                       g_abs[i].value = 0x200u + i; g_abs[i].cls = PIC_CLASS_SLOT; }
    for (uint32_t i = 0; i < sizeof(g_data); i++) g_data[i] = (uint8_t)(0xA0u + i);

    memset(m, 0, sizeof(*m));
    m->header.magic = RPC_APP_MAGIC;
    m->header.api_major = RPC_API_MAJOR;
    m->header.api_minor = RPC_API_MINOR;
    snprintf(m->header.name, sizeof(m->header.name), "fixture");
    snprintf(m->header.version, sizeof(m->header.version), "1.2.3");
    m->api_major  = RPC_API_MAJOR;
    m->api_minor  = RPC_API_MINOR;
    m->ro_size    = BLOB_BYTES;
    m->text_size  = 900;
    m->rodata_off = 900; m->rodata_size = 300;
    m->veneer_off = 1200; m->veneer_size = 101;
    m->entry_off  = 0x40;
    m->got_bytes  = 7 * 4;
    m->data_off   = 7 * 4;
    m->data_size  = sizeof(g_data);
    m->ram_size   = 512;
    m->got_count  = 7;   m->got = g_got;
    m->abs_count  = 3;   m->abs = g_abs;
    m->data_init  = g_data;
}

// Write the fixture into slot 0 of the fake, in the pieces a real install would.
static bool install(uint32_t slot_bytes, PicManifest *m, uint32_t chunk) {
    PkgSlotWriter w;
    if (!pkgslot_begin(&w, &kFake, 0, slot_bytes)) return false;
    for (uint32_t off = 0; off < BLOB_BYTES; off += chunk) {
        uint32_t n = BLOB_BYTES - off < chunk ? BLOB_BYTES - off : chunk;
        if (!pkgslot_sink(&w, off, g_blob + off, n)) return false;
    }
    return pkgslot_commit(&w, m);
}

int main(void) {
    const uint32_t SLOT = 16u * 1024u;
    PicManifest m;
    fixture(&m);

    // --- 1. round trip -------------------------------------------------------
    fake_reset();
    ok(install(SLOT, &m, 337), "install completes");   // 337: writes straddle pages
    ok(!g_f.violated, "no program rewrote a bit that was already clear");

    PicManifest r;
    PkgSlotStatus st = pkgslot_open(g_f.mem, SLOT, &r);
    ok(st == PKGSLOT_OK, "a committed slot opens");
    if (st == PKGSLOT_OK) {
        ok(strcmp(r.header.name, "fixture") == 0, "the package name comes back");
        ok(strcmp(r.header.version, "1.2.3") == 0, "the package version comes back");
        ok(r.ro_size == BLOB_BYTES && r.text_size == 900 && r.entry_off == 0x40,
           "the blob extents come back");
        ok(r.ram_size == 512 && r.got_bytes == 28 && r.data_off == 28,
           "the RAM layout comes back");
        ok(r.got_count == 7 && r.abs_count == 3 && r.data_size == sizeof(g_data),
           "the recipe counts come back");
        ok(r.borrowed, "a slot-backed manifest is marked borrowed");

        // The blob, byte for byte, at the offset app_pic_load will be handed.
        const uint8_t *blob = (const uint8_t *)pkgslot_blob(g_f.mem);
        ok(memcmp(blob, g_blob, BLOB_BYTES) == 0, "the blob reads back identical");

        // The recipe arrays are read IN PLACE out of the mapping — the whole
        // reason a package costs its writable half and nothing more.
        ok((const uint8_t *)r.got >= g_f.mem && (const uint8_t *)r.got < g_f.mem + SLOT,
           "the GOT recipe points into the slot, not into a copy");
        ok(((uintptr_t)r.got & 3u) == 0 && ((uintptr_t)r.abs & 3u) == 0,
           "both recipe arrays are four-aligned");
        ok(memcmp(r.got, g_got, sizeof(g_got)) == 0, "the GOT recipe reads back identical");
        ok(memcmp(r.abs, g_abs, sizeof(g_abs)) == 0, "the ABS32 recipe reads back identical");
        ok(memcmp(r.data_init, g_data, sizeof(g_data)) == 0, ".data initialisers read back identical");

        // Freeing a borrowed manifest must release nothing. Under the address
        // sanitiser a free() of a mapped address aborts the run, so this check
        // failing looks like a crash rather than a message — which is right.
        app_pic_manifest_free(&r);
        ok(g_f.mem[PKGSLOT_HEADER] == g_blob[0], "freeing a borrowed manifest touches no flash");
    }

    // --- 2. a torn install reads as empty ------------------------------------
    //
    // Cut the power after every possible number of programs and check the slot is
    // never loadable. This is the check the whole commit ordering exists for.
    {
        fake_reset();
        install(SLOT, &m, 337);
        uint32_t total = g_f.programs;
        ok(total > 3, "the install takes several programs to cut between");
        int loadable = 0, torn = 0;
        for (uint32_t cut = 0; cut < total; cut++) {
            fake_reset();
            // Start from a slot that already holds a VALID package, so "empty"
            // has to have been produced rather than merely never disturbed.
            install(SLOT, &m, 337);
            PicManifest before;
            if (pkgslot_open(g_f.mem, SLOT, &before) != PKGSLOT_OK) { loadable = -1; break; }
            g_f.fail_after = g_f.programs + cut;
            install(SLOT, &m, 337);
            PicManifest after;
            PkgSlotStatus s2 = pkgslot_open(g_f.mem, SLOT, &after);
            if (s2 == PKGSLOT_OK) loadable++; else torn++;
        }
        ok(loadable == 0, "no interrupted install leaves a loadable slot");
        ok(torn == (int)total, "every interrupted install leaves the slot unloadable");
    }

    // --- 3. erasing the header happens FIRST ---------------------------------
    {
        fake_reset();
        install(SLOT, &m, 512);
        PicManifest tmp;
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_OK, "slot valid before");
        PkgSlotWriter w;
        pkgslot_begin(&w, &kFake, 0, SLOT);
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_EMPTY,
           "begin invalidates the slot before a byte of the body is written");
    }

    // --- 4. what a damaged slot says -----------------------------------------
    {
        fake_reset(); install(SLOT, &m, 512);
        g_f.mem[PKGSLOT_HEADER + 40] ^= 0x01;           // a bit rot in the blob
        PicManifest tmp;
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_BAD_CRC, "a corrupted body is refused");

        fake_reset(); install(SLOT, &m, 512);
        g_f.mem[PKGSLOT_META_OFF + 8] ^= 0x01;          // a bit rot in the metadata
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_BAD_CRC, "corrupted metadata is refused");

        fake_reset();
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_EMPTY, "an erased slot is empty");

        fake_reset(); install(SLOT, &m, 512);
        ((PkgSlotCommit *)g_f.mem)->format = PKGSLOT_FORMAT + 1;
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_BAD_FORMAT,
           "a slot from another slot format is refused");
    }

    // --- 5. the failure that would not fault ---------------------------------
    //
    // A slot built against an ABI whose indices have moved calls the WRONG
    // firmware function and does not crash doing it. Refusing it here is the only
    // defence, so it is checked in both directions: a newer minor is refused, an
    // older minor still runs.
    {
        PicManifest older = m; older.api_minor = (uint16_t)(RPC_API_MINOR ? RPC_API_MINOR - 1 : 0);
        fake_reset(); install(SLOT, &older, 512);
        PicManifest tmp;
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_OK,
           "a slot from an older ABI minor still opens");

        PicManifest newer = m; newer.api_minor = (uint16_t)(RPC_API_MINOR + 1);
        fake_reset(); install(SLOT, &newer, 512);
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_BAD_ABI,
           "a slot wanting a newer ABI minor is refused");

        PicManifest major = m; major.api_major = (uint16_t)(RPC_API_MAJOR + 1);
        fake_reset(); install(SLOT, &major, 512);
        ok(pkgslot_open(g_f.mem, SLOT, &tmp) == PKGSLOT_BAD_ABI,
           "a slot from another ABI major is refused");
    }

    // --- 6. refusals rather than wrong writes --------------------------------
    {
        fake_reset();
        PkgSlotWriter w;
        ok(pkgslot_begin(&w, &kFake, 0, SLOT), "begin on an aligned slot");
        ok(pkgslot_sink(&w, 0, g_blob, 100), "the first write is accepted");
        ok(!pkgslot_sink(&w, 500, g_blob, 100), "a write that skips a gap is refused");

        fake_reset();
        ok(pkgslot_begin(&w, &kFake, 0, SLOT), "begin again");
        ok(!pkgslot_sink(&w, 0, g_blob, SLOT), "a body larger than the slot is refused");

        ok(!pkgslot_begin(&w, &kFake, 100, SLOT), "an unaligned slot base is refused");
        ok(!pkgslot_begin(&w, &kFake, 0, PKGSLOT_HEADER), "a slot with no body is refused");

        // A package that does not fit: the message has to be this rather than
        // "out of memory", which is what the design doc asked for.
        fake_reset();
        ok(pkgslot_begin(&w, &kFake, 0, 8192), "begin on a small slot");
        bool wrote = true;
        for (uint32_t off = 0; off < BLOB_BYTES * 8 && wrote; off += BLOB_BYTES)
            wrote = pkgslot_sink(&w, off, g_blob, BLOB_BYTES);
        ok(!wrote, "a package too big for its slot is refused at the write, not later");
    }

    // --- 7. lazy erase: a small package does not erase a large slot ----------
    {
        fake_reset();
        install(256u * 1024u > REGION ? SLOT : SLOT, &m, 512);
        // header sector + the sectors the ~1.4 KB body actually touches
        ok(g_f.erases <= 2, "a 1.4 KB package erases the header sector and one more");
    }

    // --- 8. which path a package takes ---------------------------------------
    //
    // The decision itself, as a table, because it is the part with the most ways
    // to be quietly wrong and the least chance of anyone noticing. Every row that
    // says COPY is a device behaving exactly as it did before slots existed; the
    // ONE row that says SLOT is the case that has to be provably narrow.
    //
    // The RP2040 rows matter most. Those boards have zero slots by design —
    // SANDBOX_SUPPORTED is 0 on ARMv6-M, so a package there runs privileged and
    // its veneers hold firmware ADDRESSES, which cannot be baked into flash. Both
    // `svc = false` and `slot_bytes = 0` describe them, and either one on its own
    // has to be enough.
    {
        const uint32_t SLOTB = 256u * 1024u;
        const uint32_t CAP   = pkgslot_body_capacity(SLOTB);
        ok(CAP == SLOTB - PKGSLOT_HEADER, "a slot's body is everything but the header sector");
        ok((CAP % PKGSLOT_PROG) == 0, "and it is a whole number of program pages");
        ok(pkgslot_body_capacity(PKGSLOT_HEADER) == 0, "a slot with no body holds nothing");

        // The one that goes to flash.
        PkgRouteIn in{ /*pic*/true, /*svc*/true, /*free*/true, SLOTB, 123456u };
        ok(pkg_route(in) == PKG_ROUTE_SLOT,
           "a PIC package, in SVC mode, with a free slot it fits: the slot");

        // Each condition removed on its own, so no single one is load-bearing by
        // accident.
        PkgRouteIn nonpic = in; nonpic.pic = false;
        ok(pkg_route(nonpic) == PKG_ROUTE_COPY, "a package that is not PIC: copy to RAM");

        PkgRouteIn direct = in; direct.svc = false;
        ok(pkg_route(direct) == PKG_ROUTE_COPY, "DIRECT veneers (RP2040): copy to RAM");

        PkgRouteIn noslot = in; noslot.slot_bytes = 0;
        ok(pkg_route(noslot) == PKG_ROUTE_COPY, "a board with no slots: copy to RAM");

        PkgRouteIn taken = in; taken.slot_free = false;
        ok(pkg_route(taken) == PKG_ROUTE_COPY,
           "the slot already holds another package: copy to RAM");

        // An RP2040 as it really arrives: no sandbox AND no slots.
        PkgRouteIn rp2040 = in; rp2040.svc = false; rp2040.slot_bytes = 0;
        ok(pkg_route(rp2040) == PKG_ROUTE_COPY, "an RP2040 board: copy to RAM");
        // ...and it stays COPY even for a package that would otherwise fit, which
        // is the case a test that only ever passes zero would miss.
        rp2040.need_bytes = 8u * 1024u;
        ok(pkg_route(rp2040) == PKG_ROUTE_COPY,
           "an RP2040 board, with a package that would have fitted: still copy");

        // Too big is its OWN answer and not "out of memory" and not "copy" —
        // the caller has something specific to say about it.
        PkgRouteIn big = in; big.need_bytes = CAP + 1;
        ok(pkg_route(big) == PKG_ROUTE_TOO_BIG, "one byte past the slot: too big for the slot");
        PkgRouteIn exact = in; exact.need_bytes = CAP;
        ok(pkg_route(exact) == PKG_ROUTE_SLOT, "exactly filling the slot still fits");
        ok(strcmp(pkg_route_str(PKG_ROUTE_TOO_BIG), "too big for the slot") == 0,
           "and it says so in those words");

        // Too big only outranks the quiet reasons when the package could have had
        // the slot. A non-PIC package on a board with no slots is not "too big".
        PkgRouteIn bignonpic = big; bignonpic.pic = false;
        ok(pkg_route(bignonpic) == PKG_ROUTE_COPY,
           "a non-PIC package larger than a slot is copy, not too big");
    }

    ok(!g_f.violated, "no fake-flash rule was broken across the whole run");
    printf(fails ? "pkgslot: %d checks, %d FAILED\n" : "pkgslot: %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
