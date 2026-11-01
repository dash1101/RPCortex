// Desc: The .sub / .ir / .nfc codecs against the MicroPython ones that wrote them first.
// File: novacodec_test.cpp
//
// codec_vectors.inc is what novacc.py, novair.py and novanfc.py answered for a few
// hundred inputs (gen_codec_vectors.py); this runs the C++ ports on the same inputs
// and holds every number and every byte of text to it. Then it adds what the
// MicroPython modules could not be asked, because they had no limits: output that
// does not fit, a sink that fills, and a file with more durations than the caller's
// array.
#include "../apps/novad1/novasub.cpp"
#include "../apps/novad1/novair.cpp"
#include "../apps/novad1/novanfc.cpp"
#include "../apps/novad1/novaoui.cpp"
#include "../apps/novad1/novatotp.cpp"
#include "../apps/novad1/novagps.cpp"
#include "../apps/novad1/novawigle.cpp"
#include "../apps/novad1/novanfcdump.cpp"

#include <stdio.h>
#include <string.h>

#include "codec_vectors.inc"

using namespace nova;
using codec::Sink;

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 25) { printf("    FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// A sink that writes into a buffer and refuses what does not fit.
struct Buf {
    char     b[16384];
    unsigned n;
    unsigned cap;
};
static bool buf_put(void *ctx, const char *s, unsigned n) {
    Buf *x = (Buf *)ctx;
    if (x->n + n >= x->cap) return false;
    memcpy(x->b + x->n, s, n);
    x->n += n;
    x->b[x->n] = 0;
    return true;
}
static void buf_init(Buf *x, unsigned cap = sizeof(x->b)) { x->n = 0; x->b[0] = 0; x->cap = cap; }
static Sink sink_of(Buf *x) { return Sink{ buf_put, x }; }

static uint32_t g_t[4096];

static void test_sub_encode(void) {
    int bad = 0;
    for (unsigned i = 0; i < SUB_ENC_N; i++) {
        const SubEncCase &c = kSubEnc[i];
        unsigned n = sub::encode_decoded(c.proto, c.key, c.bits, c.te, g_t, 4096);
        bool same = (n == c.n);
        for (unsigned j = 0; same && j < n; j++) same = (g_t[j] == c.t[j]);
        if (!same) {
            if (bad++ < 5) printf("    .sub encode '%s' key '%s' %d bits te %d: got %u values, want %u\n", c.proto, c.key, c.bits, c.te, n, c.n);
        }
    }
    CHECK(bad == 0, "%d of %u .sub encodes differ from the MicroPython codec", bad, (unsigned)SUB_ENC_N);
    // The output has to fit: one value short of enough is a refusal, not a truncation.
    unsigned full = sub::encode_decoded("Princeton", "AB CD EF", 24, 350, g_t, 4096);
    CHECK(full == 50, "Princeton 24 bits is 50 durations (got %u)", full);
    CHECK(sub::encode_decoded("Princeton", "AB CD EF", 24, 350, g_t, 49) == 0, "a short output array is refused");
    CHECK(sub::encode_decoded("Princeton", "AB", 65, 350, g_t, 4096) == 0, "more than 64 bits is refused");
}

static void test_sub_parse(void) {
    static uint32_t raw[4096];
    for (unsigned i = 0; i < SUB_PARSE_N; i++) {
        const SubParseCase &c = kSubParse[i];
        sub::File f;
        unsigned n = 0, dropped = 0;
        sub::parse(c.text, &f, raw, 4096, &n, &dropped);
        CHECK(f.freq_hz == c.freq_hz, "parse %u: frequency %u, want %u", i, f.freq_hz, c.freq_hz);
        CHECK(!strcmp(f.preset, c.preset), "parse %u: preset '%s', want '%s'", i, f.preset, c.preset);
        CHECK(!strcmp(f.protocol, c.protocol), "parse %u: protocol '%s', want '%s'", i, f.protocol, c.protocol);
        CHECK((int)f.has_key == c.has_key, "parse %u: has_key %d, want %d", i, f.has_key, c.has_key);
        if (c.has_key) CHECK(!strcmp(f.key, c.key), "parse %u: key '%s', want '%s'", i, f.key, c.key);
        CHECK(f.bits == c.bits, "parse %u: bits %d, want %d", i, f.bits, c.bits);
        CHECK(f.te == c.te, "parse %u: te %d, want %d", i, f.te, c.te);
        bool same = (n == c.n);
        for (unsigned j = 0; same && j < n; j++) same = (raw[j] == c.raw[j]);
        CHECK(same, "parse %u: %u raw values, want %u (or a value differs)", i, n, c.n);
        CHECK(dropped == 0, "parse %u: dropped %u", i, dropped);
    }
    // More values than the array holds: the count that did not fit is reported.
    const char *big = "RAW_Data: 1 -2 3 -4 5 -6 7\nRAW_Data: 8 -9\n";
    sub::File f;
    unsigned n = 0, dropped = 0;
    sub::parse(big, &f, raw, 4, &n, &dropped);
    CHECK(n == 4 && dropped == 5, "a capture bigger than the array reports what it dropped (n %u dropped %u)", n, dropped);
}

static void test_sub_write(void) {
    for (unsigned i = 0; i < SUB_WRITE_N; i++) {
        const SubWriteCase &c = kSubWrite[i];
        Buf b; buf_init(&b);
        bool ok = sub::write_raw(sink_of(&b), c.t, c.n, c.freq_hz, nullptr);
        CHECK(ok, "write %u: the sink refused", i);
        CHECK(!strcmp(b.b, c.expect), "write %u: text differs from the MicroPython file (%u bytes, want %u)", i, b.n, (unsigned)strlen(c.expect));
    }
    // A sink that fills stops the writer and says so.
    Buf b; buf_init(&b, 64);
    CHECK(!sub::write_raw(sink_of(&b), kSubWrite[0].t, kSubWrite[0].n, 433920000, nullptr), "a full sink fails the write");

    // Parsing what was written gives back what went in.
    static uint32_t raw[4096];
    sub::File f; unsigned n = 0;
    buf_init(&b);
    sub::write_raw(sink_of(&b), kSubWrite[1].t, kSubWrite[1].n, 315000000, "FuriHalSubGhzPresetOok270Async");
    sub::parse(b.b, &f, raw, 4096, &n, nullptr);
    bool same = n == kSubWrite[1].n;
    for (unsigned j = 0; same && j < n; j++) same = raw[j] == kSubWrite[1].t[j];
    CHECK(same && f.freq_hz == 315000000 && !strcmp(f.preset, "FuriHalSubGhzPresetOok270Async") && !strcmp(f.protocol, "RAW"),
          "a written file parses back to what was written");
}

static void test_ir_encode(void) {
    int bad = 0;
    for (unsigned i = 0; i < IR_ENC_N; i++) {
        const IrEncCase &c = kIrEnc[i];
        uint32_t freq = 0;
        unsigned n = ir::encode(c.proto, c.addr, c.cmd, &freq, g_t, 4096);
        bool same = (n == c.n) && (!n || freq == c.freq);
        for (unsigned j = 0; same && j < n; j++) same = (g_t[j] == c.t[j]);
        if (!same && bad++ < 5)
            printf("    .ir encode '%s' addr '%s' cmd '%s': got %u values at %u Hz, want %u at %u Hz\n", c.proto, c.addr, c.cmd, n, freq, c.n, c.freq);
    }
    CHECK(bad == 0, "%d of %u .ir encodes differ from the MicroPython codec", bad, (unsigned)IR_ENC_N);
    uint32_t f;
    CHECK(ir::encode("NEC", "04", "08", &f, g_t, 10) == 0, "a short output array is refused");
}

static void test_ir_parse(void) {
    for (unsigned i = 0; i < IR_PARSE_N; i++) {
        const IrParseCase &c = kIrParse[i];
        const char *cur = c.text;
        ir::Signal sig;
        unsigned got = 0;
        while (ir::next(&cur, &sig, g_t, 4096)) {
            if (got < c.count) {
                const IrSigExpect &e = c.sigs[got];
                CHECK(!strcmp(sig.name, e.name), "ir parse %u signal %u: name '%s', want '%s'", i, got, sig.name, e.name);
                CHECK(sig.freq_hz == e.freq, "ir parse %u signal %u: %u Hz, want %u", i, got, sig.freq_hz, e.freq);
                CHECK(sig.duty_ppm == e.duty_ppm, "ir parse %u signal %u: duty %u, want %u", i, got, sig.duty_ppm, e.duty_ppm);
                bool same = sig.n == e.n;
                for (unsigned j = 0; same && j < sig.n; j++) same = g_t[j] == e.t[j];
                CHECK(same, "ir parse %u signal %u: %u durations, want %u (or one differs)", i, got, sig.n, e.n);
            }
            got++;
            if (got > 20) break;
        }
        CHECK(got == c.count, "ir parse %u: %u signals, want %u", i, got, c.count);
    }
    // A signal longer than the array is cut and says so.
    const char *long_sig = "name: Long\ntype: raw\ndata: 1 2 3 4 5 6 7 8\n";
    const char *cur = long_sig;
    ir::Signal sig;
    CHECK(ir::next(&cur, &sig, g_t, 5) && sig.n == 5 && sig.truncated, "a signal past the array is truncated and flagged");
}

static void test_ir_write(void) {
    Buf b; buf_init(&b);
    ir::write_header(sink_of(&b));
    ir::write_signal(sink_of(&b), "Power", ir_write_t1, 7, 38000, 330000);
    CHECK(!strcmp(b.b, kIrWriteFirst), "a new .ir file matches the MicroPython one\n%s", b.b);
    ir::write_signal(sink_of(&b), "Mute", ir_write_t2, 4, 36000, 250000);
    CHECK(!strcmp(b.b, kIrWriteBoth), "an appended signal matches the MicroPython one");

    // And a file written here reads back signal for signal.
    const char *cur = b.b;
    ir::Signal s;
    int count = 0;
    while (ir::next(&cur, &s, g_t, 4096)) count++;
    CHECK(count == 2, "a written .ir reads back as two signals (got %d)", count);
}

// --- NFC ------------------------------------------------------------------------------------

struct PageCtx { int kind; };      // 0 pages, 1 blocks
static bool page_get(void *ctx, unsigned i, uint8_t *out, uint16_t *) {
    (void)ctx;
    if (i == 3) return false;
    for (unsigned k = 0; k < 4; k++) out[k] = (uint8_t)(i + k);
    return true;
}
static bool block_get(void *, unsigned i, uint8_t *out, uint16_t *) {
    if (i == 5) return false;
    for (unsigned k = 0; k < 16; k++) out[k] = (uint8_t)(i + k);
    return true;
}
// Blocks that are only partly known: a trailer whose Key A is hidden, and one whose Key B is.
static bool partial_get(void *, unsigned i, uint8_t *out, uint16_t *known) {
    for (unsigned k = 0; k < 16; k++) out[k] = (uint8_t)(i + k);
    if (i == 3) { out[6] = 0x7F; out[7] = 0x07; out[8] = 0x88; out[9] = 0x69; *known = 0xFFC0; }       // A unknown
    if (i == 7) { *known = 0x03FF; }                                                                  // B unknown
    return true;
}

static void test_nfc(void) {
    const uint8_t uid7[7] = { 0x04, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7 };
    const uint8_t uid4[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t atqa_ntag[2] = { 0x00, 0x44 }, atqa_classic[2] = { 0x00, 0x04 };

    Buf b; buf_init(&b);
    nfc::write_iso14443a(sink_of(&b), uid7, 7, atqa_ntag, 0x20);
    CHECK(!strcmp(b.b, kNfcIso), "an identified card matches the MicroPython file");

    buf_init(&b);
    nfc::write_ultralight(sink_of(&b), uid7, 7, atqa_ntag, 0x00, "NTAG213", 45, page_get, nullptr, nullptr);
    CHECK(!strcmp(b.b, kNfcUl), "an NTAG dump matches the MicroPython file (%u bytes, want %u)", b.n, (unsigned)strlen(kNfcUl));

    nfc::UltralightExtras x = {};
    uint8_t sig[32], ver[8];
    for (int i = 0; i < 32; i++) sig[i] = (uint8_t)i;
    for (int i = 0; i < 8; i++) ver[i] = (uint8_t)i;
    x.signature = sig; x.mifare_version = ver;
    x.counters[0] = 1; x.counters[1] = 70000; x.counters[2] = 3;
    x.tearing[0] = 0xBD; x.tearing[1] = 0; x.tearing[2] = 0x7F;
    x.pages_read = 10;
    buf_init(&b);
    nfc::write_ultralight(sink_of(&b), uid7, 7, atqa_ntag, 0x00, "NTAG215", 16, page_get, nullptr, &x);
    CHECK(!strcmp(b.b, kNfcUl2), "an NTAG with its signature and counters matches the MicroPython file");

    buf_init(&b);
    nfc::write_classic(sink_of(&b), uid4, 4, atqa_classic, 0x08, "1K", 64, block_get, nullptr);
    CHECK(!strcmp(b.b, kNfcMc), "a Mifare Classic 1K matches the MicroPython file (%u bytes, want %u)", b.n, (unsigned)strlen(kNfcMc));

    buf_init(&b);
    nfc::write_classic(sink_of(&b), uid4, 4, atqa_classic, 0x08, "1K", 8, partial_get, nullptr);
    CHECK(!strcmp(b.b, kNfcMcPartial), "blocks that are partly known match the MicroPython file (got '%s')", b.b);

    // Read the identity back out of each.
    nfc::Info in;
    CHECK(nfc::read_info(kNfcUl, &in) && in.uid_len == 7 && !memcmp(in.uid, uid7, 7) && in.atqa_len == 2 &&
          in.atqa[0] == 0x00 && in.atqa[1] == 0x44 && in.sak == 0x00 && !strcmp(in.device_type, "NTAG/Ultralight") && in.memory_lines == 45,
          "an NTAG file reads back its identity and its 45 pages (got %u lines)", in.memory_lines);
    CHECK(nfc::read_info(kNfcMc, &in) && in.uid_len == 4 && in.sak == 0x08 && in.memory_lines == 64 && !strcmp(in.device_type, "Mifare Classic"),
          "a Classic file reads back its identity and 64 blocks");
    CHECK(nfc::read_info(kNfcIso, &in) && in.sak == 0x20 && in.memory_lines == 0, "an identified-only card has no memory lines");
    CHECK(!nfc::read_info("not a card\n# comment\n", &in), "a file with no UID is not a card");

    char v[64];
    CHECK(nfc::get(kNfcMc, "Mifare Classic type", v, sizeof(v)) && !strcmp(v, "1K"), "a key is found by name");
    CHECK(!nfc::get(kNfcMc, "Nope", v, sizeof(v)) && v[0] == 0, "an absent key is absent");
    // A key is exact: "Block" is not "Block 5".
    CHECK(!nfc::get(kNfcMc, "Block", v, sizeof(v)), "a key matches whole, not by prefix");

    uint8_t bytes[8];
    CHECK(nfc::unhex("04 A2:b3 ?? 0", bytes, 8) == 4 && bytes[0] == 0x04 && bytes[1] == 0xA2 && bytes[2] == 0xB3 && bytes[3] == 0,
          "unhex reads colons as separators, ?? as zero, and drops an odd trailing digit");

    for (unsigned i = 0; i < NFC_IDENT_N; i++) {
        const NfcIdentCase &c = kNfcIdent[i];
        nfc::Kind k = nfc::identify((uint8_t)c.sak, c.atqa, c.atqa_len, c.pages);
        CHECK(!strcmp(k.device_type, c.dt) && ((k.sub_type == nullptr) == (c.sub == nullptr)) &&
              (!c.sub || !strcmp(k.sub_type, c.sub)),
              "identify SAK %02X ATQA %02X %02X pages %u: got %s / %s, want %s / %s", c.sak, c.atqa[0], c.atqa[1], c.pages,
              k.device_type, k.sub_type ? k.sub_type : "-", c.dt, c.sub ? c.sub : "-");
    }
    for (unsigned i = 0; i < NFC_COUNT_N; i++)
        CHECK(nfc::classic_block_count(kNfcCount[i].mc) == kNfcCount[i].n, "block count for '%s'", kNfcCount[i].mc);

    // A sink that fills stops the writer.
    buf_init(&b, 100);
    CHECK(!nfc::write_classic(sink_of(&b), uid4, 4, atqa_classic, 0x08, "1K", 64, block_get, nullptr), "a full sink fails the dump");
}

// --- the NFC dump engine, against a model of a card ------------------------------------------
//
// A fake PN532 with a card on it: it answers select and the two things a dump sends an
// InDataExchange - a Mifare authenticate and a block/page read - the way a real one would, so the
// walk in novanfcdump can be driven to the end with no reader attached. Two cards: a 1K Classic
// where one sector uses a non-default key (so a "??" sector is exercised), and an NTAG215.
using nova::nfcdump::Tag;
using nova::nfcdump::Reader;
using nova::nfcdump::Dump;

struct FakeCard {
    int      kind;              // 0 = classic 1K, 1 = ntag215
    uint8_t  sak;
    uint8_t  atqa[2];
    uint8_t  uid[10];
    unsigned uid_len;
    bool     authed;
    unsigned auth_block;
    int      selects, exchanges, field_resets;
    unsigned locked_sector;     // a Classic sector no default key opens; ~0 for none
    unsigned read_cap;          // pages/blocks that still read; the rest fail (a torn dump)
    unsigned short_block;       // a Classic block whose read returns too few bytes; ~0 for none
};

static const uint8_t kFFKey[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int fake_select(void *ctx, Tag *out, bool reset) {
    FakeCard *c = (FakeCard *)ctx;
    c->selects++;
    if (reset) { c->field_resets++; c->authed = false; }
    memset(out, 0, sizeof(*out));
    memcpy(out->uid, c->uid, c->uid_len);
    out->uid_len = (uint8_t)c->uid_len;
    out->atqa[0] = c->atqa[0]; out->atqa[1] = c->atqa[1];
    out->sak = c->sak;
    return 1;
}

static int fake_exchange(void *ctx, const uint8_t *tx, unsigned tx_len, uint8_t *rx, unsigned rx_cap) {
    FakeCard *c = (FakeCard *)ctx;
    c->exchanges++;
    if (tx_len == 0) return -1;

    if (c->kind == 0) {
        // Classic: authenticate (0x60/0x61, block, key[6], uid[4]) then read (0x30, block).
        if ((tx[0] == 0x60 || tx[0] == 0x61) && tx_len == 12) {
            const unsigned block = tx[1];
            const unsigned sector = block < 128 ? block / 4u : 32u + (block - 128u) / 16u;
            if (sector == c->locked_sector) return (-(256 + 0x14));   // no key here opens it
            if (memcmp(tx + 2, kFFKey, 6) != 0) return (-(256 + 0x14));   // only FFs work on this card
            c->authed = true; c->auth_block = block;
            return 0;                                                     // auth replies with no data
        }
        if (tx[0] == 0x30 && tx_len == 2) {
            const unsigned block = tx[1];
            const unsigned nblk = (c->sak == 0x18 || c->sak == 0x98) ? 256u : 64u;
            if (!c->authed) return (-(256 + 0x13));
            if (block >= nblk) return (-(256 + 0x01));
            if (rx_cap < 16) return 0;
            if (block == c->short_block) { memset(rx, 0xEE, 10); return 10; }   // a torn read: too few bytes
            const bool trailer = (block < 128) ? ((block % 4) == 3) : (((block - 128) % 16) == 15);
            // Contents: block number in every byte, except a trailer, which is Key A / access / Key B.
            if (trailer) {
                memset(rx, 0x00, 6);                          // Key A reads back as zeros on a real card
                rx[6] = 0xFF; rx[7] = 0x07; rx[8] = 0x80; rx[9] = 0x69;
                memset(rx + 10, 0x00, 6);                     // Key B also zeros in transport config
            } else {
                memset(rx, (uint8_t)block, 16);
            }
            return 16;
        }
        return (-(256 + 0x01));
    }

    // NTAG215: GET_VERSION (0x60), READ_SIG (0x3C 0x00), READ (0x30 page).
    if (tx[0] == 0x60 && tx_len == 1) {
        static const uint8_t ver[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03 };
        if (rx_cap < 8) return 0;
        memcpy(rx, ver, 8);
        return 8;
    }
    if (tx[0] == 0x3C && tx_len == 2) {
        if (rx_cap < 32) return 0;
        for (int i = 0; i < 32; i++) rx[i] = (uint8_t)(0x40 + i);
        return 32;
    }
    if (tx[0] == 0x30 && tx_len == 2) {
        const unsigned page = tx[1];
        if (page / 4 == c->short_block) { memset(rx, 0x00, 8); return 8; }   // a short read ends the walk
        if (page / 4 >= c->read_cap) return (-(256 + 0x01));   // past where the card still answers
        if (rx_cap < 16) return 0;
        for (int i = 0; i < 16; i++) rx[i] = (uint8_t)(page + i);   // four pages at once
        return 16;
    }
    return (-(256 + 0x01));
}

static bool g_dump_cancel;
static unsigned g_dump_last_done, g_dump_last_total;
static bool fake_cancelled(void *) { return g_dump_cancel; }
static void fake_progress(void *, unsigned done, unsigned total) { g_dump_last_done = done; g_dump_last_total = total; }

static Reader reader_for(FakeCard *c) {
    Reader r;
    r.select = fake_select; r.exchange = fake_exchange;
    r.cancelled = fake_cancelled; r.progress = fake_progress; r.ctx = c;
    return r;
}

static void test_nfc_dump(void) {
    using namespace nova;

    // --- a Classic 1K, sector 5 locked. ---
    FakeCard c = {};
    c.kind = 0; c.sak = 0x08; c.atqa[0] = 0x00; c.atqa[1] = 0x04;
    memcpy(c.uid, "\xDE\xAD\xBE\xEF", 4); c.uid_len = 4;
    c.locked_sector = 5;
    c.short_block = 7;          // one block answers with too few bytes - must be left unknown
    g_dump_cancel = false; g_dump_last_done = g_dump_last_total = 0;

    static nfcdump::Dump d;
    Tag first; fake_select(&c, &first, false); c.selects = 0;
    CHECK(nfcdump::run(reader_for(&c), first, &d) == nfcdump::DUMP_OK, "a Classic 1K dumps");
    CHECK(d.type == nfcdump::DUMP_CLASSIC && !strcmp(d.kind, "1K") && d.units == 64, "as 64 blocks");
    CHECK(d.read_units == 59, "59 read: not the locked sector's 4, nor the one short read (got %u)", d.read_units);
    CHECK(d.known[7] == 0, "a block that read too few bytes is left unknown, not half-filled");
    CHECK(g_dump_last_done == 64 && g_dump_last_total == 64, "and progress reaches the end");
    CHECK(c.field_resets > 0, "the field was reset to clear a halt between keys");

    // A normal block is its own number; a trailer carries the key that opened it with the other half ??.
    CHECK(d.known[1] == 0xFFFF && d.mem[1 * 16] == 1, "a data block is fully known");
    CHECK(d.mem[3 * 16 + 0] == 0xFF && d.mem[3 * 16 + 5] == 0xFF, "a trailer keeps the Key A that opened it");
    CHECK((d.known[3] & 0xFC00) == 0, "and Key B's six bytes are not trusted");
    CHECK(d.mem[3 * 16 + 6] == 0xFF && d.mem[3 * 16 + 8] == 0x80, "with the access bits as read");
    CHECK(d.known[20] == 0 && d.known[23] == 0, "the locked sector's blocks are all unknown");

    // The dump writes a .nfc the reader accepts, with the locked sector as "??".
    Buf b; buf_init(&b);
    bool w = nfc::write_classic(sink_of(&b), d.tag.uid, d.tag.uid_len, d.tag.atqa, d.tag.sak, d.kind,
                                d.units, nfcdump::unit_getter, &d);
    CHECK(w, "the Classic dump writes a .nfc file");
    nfc::Info in;
    CHECK(nfc::read_info(b.b, &in) && in.memory_lines == 64 && !strcmp(in.device_type, "Mifare Classic"),
          "which reads back as a 64-block Classic");
    CHECK(strstr(b.b, "Block 20: ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??") != nullptr,
          "the locked sector is all ?? in the file");
    CHECK(strstr(b.b, "Block 3: FF FF FF FF FF FF FF 07 80 69 ?? ?? ?? ?? ?? ??") != nullptr,
          "and a trailer shows the key, the access bits, and ?? for the untrusted half");

    // --- cancel partway. ---
    FakeCard c2 = c; c2.selects = c2.exchanges = c2.field_resets = 0;
    g_dump_cancel = true;
    static nfcdump::Dump d2;
    Tag f2; fake_select(&c2, &f2, false);
    CHECK(nfcdump::run(reader_for(&c2), f2, &d2) == nfcdump::DUMP_CANCELLED, "a cancel stops the dump");

    // --- a Classic 4K: the eight 16-block sectors at the top are the geometry that catches an
    //     off-by-one in sector_first / sector_blocks. ---
    FakeCard c4 = {};
    c4.kind = 0; c4.sak = 0x18; c4.atqa[0] = 0x00; c4.atqa[1] = 0x02;
    memcpy(c4.uid, "\x11\x22\x33\x44", 4); c4.uid_len = 4;
    c4.locked_sector = ~0u; c4.short_block = ~0u;
    g_dump_cancel = false;
    static nfcdump::Dump d4;
    Tag f4; fake_select(&c4, &f4, false);
    CHECK(nfcdump::run(reader_for(&c4), f4, &d4) == nfcdump::DUMP_OK, "a Classic 4K dumps");
    CHECK(!strcmp(d4.kind, "4K") && d4.units == 256, "as 256 blocks");
    CHECK(d4.read_units == 256, "all of them read (got %u)", d4.read_units);
    CHECK(d4.mem[200 * 16] == 200, "a block in the big-sector region is itself");
    CHECK(g_dump_last_done == 256 && g_dump_last_total == 256, "and progress reaches 256");
    // The last block of sector 39 is block 255, a trailer in a 16-block sector.
    CHECK(d4.mem[255 * 16] == 0xFF && (d4.known[255] & 0xFC00) == 0, "the top trailer carries its key, Key B ??");

    // --- an NTAG215, torn after 30 pages. ---
    FakeCard n = {};
    n.kind = 1; n.sak = 0x00; n.atqa[0] = 0x00; n.atqa[1] = 0x44;
    memcpy(n.uid, "\x04\xA2\xB3\xC4\xD5\xE6\xF7", 7); n.uid_len = 7;
    n.read_cap = 999; n.short_block = ~0u;
    g_dump_cancel = false;
    static nfcdump::Dump dn;
    Tag fn; fake_select(&n, &fn, false);
    CHECK(nfcdump::run(reader_for(&n), fn, &dn) == nfcdump::DUMP_OK, "an NTAG215 dumps");
    CHECK(dn.type == nfcdump::DUMP_ULTRALIGHT && !strcmp(dn.kind, "NTAG215") && dn.units == 135,
          "as NTAG215, 135 pages");
    CHECK(dn.has_ver && dn.has_sig, "with its version and signature");
    CHECK(dn.read_units == 135 && dn.known[0] == 0x000F && dn.mem[4] == 4, "every page read, four bytes each");

    buf_init(&b);
    nfc::UltralightExtras x = {};
    x.signature = dn.sig; x.mifare_version = dn.mifare_version;
    w = nfc::write_ultralight(sink_of(&b), dn.tag.uid, dn.tag.uid_len, dn.tag.atqa, dn.tag.sak, dn.kind,
                              dn.units, nfcdump::unit_getter, &dn, &x);
    CHECK(w && nfc::read_info(b.b, &in) && in.memory_lines == 135, "the NTAG dump writes a 135-page .nfc");

    // A tear partway: the pages after it are "??", and the file still reads.
    FakeCard nt = n; nt.read_cap = 8;        // only the first 32 pages answer
    static nfcdump::Dump dt;
    Tag ft; fake_select(&nt, &ft, false);
    nfcdump::run(reader_for(&nt), ft, &dt);
    CHECK(dt.read_units == 32 && dt.units == 135, "a torn NTAG keeps what it read (%u pages)", dt.read_units);
    CHECK(dt.known[0] == 0x000F && dt.known[40] == 0, "and the pages past the tear are unknown");
    buf_init(&b);
    w = nfc::write_ultralight(sink_of(&b), dt.tag.uid, dt.tag.uid_len, dt.tag.atqa, dt.tag.sak, dt.kind,
                              dt.units, nfcdump::unit_getter, &dt, &x);
    CHECK(w && strstr(b.b, "Page 100: ?? ?? ?? ??") != nullptr, "the unread pages are ?? in the file");

    // A SHORT read (not an error) must also end the walk, or a half-page would be taken as whole.
    FakeCard ns = n; ns.short_block = 10;    // the read of pages 40-43 returns 8 bytes
    static nfcdump::Dump ds;
    Tag fs; fake_select(&ns, &fs, false);
    nfcdump::run(reader_for(&ns), fs, &ds);
    CHECK(ds.read_units == 40 && ds.known[40] == 0, "a short page read ends the dump cleanly (%u pages)", ds.read_units);

    // --- an unsupported card: identity only. ---
    FakeCard u = {};
    u.kind = 0; u.sak = 0x20; u.atqa[0] = 0x03; u.atqa[1] = 0x44; memcpy(u.uid, "\x01\x02\x03\x04", 4); u.uid_len = 4;
    static nfcdump::Dump du;
    Tag fu; fake_select(&u, &fu, false);
    CHECK(nfcdump::run(reader_for(&u), fu, &du) == nfcdump::DUMP_UNSUPPORTED, "an unknown card is not dumped");
    CHECK(du.units == 0 && du.tag.sak == 0x20, "but its identity is kept");
}

static void test_oui(void) {
    for (unsigned i = 0; i < OUI_CASES_N; i++) {
        const OuiCase &c = kOui[i];
        oui::Result r = oui::lookup(c.mac);
        bool same = ((r.vendor == nullptr) == (c.vendor == nullptr)) && ((r.klass == nullptr) == (c.klass == nullptr)) &&
                    (!c.vendor || !strcmp(r.vendor, c.vendor)) && (!c.klass || !strcmp(r.klass, c.klass));
        CHECK(same, "oui '%s': got %s/%s, want %s/%s", c.mac, r.vendor ? r.vendor : "-", r.klass ? r.klass : "-",
              c.vendor ? c.vendor : "-", c.klass ? c.klass : "-");
        char p[9];
        oui::prefix(c.mac, p, sizeof(p));
        CHECK(!strcmp(p, c.prefix), "oui prefix of '%s': '%s', want '%s'", c.mac, p, c.prefix);
    }
    for (unsigned i = 0; i < OUI_BYTE_CASES_N; i++) {
        const OuiByteCase &c = kOuiBytes[i];
        oui::Result r = oui::lookup_bytes(c.b, c.n);
        bool same = ((r.vendor == nullptr) == (c.vendor == nullptr)) && ((r.klass == nullptr) == (c.klass == nullptr)) &&
                    (!c.vendor || !strcmp(r.vendor, c.vendor)) && (!c.klass || !strcmp(r.klass, c.klass));
        CHECK(same, "oui bytes case %u", i);
    }
    // Every record in the table finds itself: the binary search reaches both ends and the middle.
    int missed = 0;
    for (unsigned i = 0; i < oui::table_size(); i++) {
        uint8_t m[3] = { oui::kOuiTable[i * 4 + 0], oui::kOuiTable[i * 4 + 1], oui::kOuiTable[i * 4 + 2] };
        // A prefix whose first octet has bit 1 set is never looked up, so only the ones
        // that are globally administered can find themselves.
        if (oui::kOuiTable[i * 4 + 0] & 0x02) continue;
        oui::Result r = oui::lookup_bytes(m, 3);
        if (!r.vendor) missed++;
    }
    CHECK(missed == 0, "%d table prefixes were not found by their own lookup", missed);
}

// --- one-time passcodes ---------------------------------------------------------------------

static unsigned from_hex(const char *h, uint8_t *out, unsigned cap) {
    unsigned n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        uint64_t v;
        codec::parse_hex(h, h + 2, &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}
static void to_hex(const uint8_t *b, unsigned n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
    out[n * 2] = 0;
}

static void test_totp(void) {
    uint8_t d[20];
    char hex[64];

    // FIPS 180: the three published messages, and a million 'a' fed in pieces.
    {
        totp::Sha1 s; totp::sha1_begin(&s); totp::sha1_end(&s, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "sha1 of nothing (got %s)", hex);
        totp::sha1_begin(&s); totp::sha1_add(&s, "abc", 3); totp::sha1_end(&s, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1 of abc (got %s)", hex);
        const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        totp::sha1_begin(&s); totp::sha1_add(&s, two, (unsigned)strlen(two)); totp::sha1_end(&s, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "sha1 of the 448-bit message (got %s)", hex);
        totp::sha1_begin(&s);
        char a[1000]; memset(a, 'a', sizeof(a));
        for (int i = 0; i < 1000; i++) totp::sha1_add(&s, a, sizeof(a));
        totp::sha1_end(&s, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "sha1 of a million a (got %s)", hex);
        // However the input is cut up, the answer is the same.
        totp::sha1_begin(&s);
        for (int i = 0; i < 100; i++) totp::sha1_add(&s, "abcdefghij", i % 11 > 10 ? 10 : (unsigned)(i % 11));
        totp::Sha1 t; totp::sha1_begin(&t);
        for (int i = 0; i < 100; i++) totp::sha1_add(&t, "abcdefghij", (unsigned)(i % 11));
        uint8_t d2[20]; totp::sha1_end(&s, d); totp::sha1_end(&t, d2);
        CHECK(!memcmp(d, d2, 20), "sha1 does not depend on how the input is cut");
    }
    for (unsigned i = 0; i < SHA1_N; i++) {
        uint8_t in[400];
        unsigned n = from_hex(kSha1[i].hex_in, in, sizeof(in));
        totp::Sha1 s; totp::sha1_begin(&s); totp::sha1_add(&s, in, n); totp::sha1_end(&s, d); to_hex(d, 20, hex);
        CHECK(n == kSha1[i].n && !strcmp(hex, kSha1[i].digest), "sha1 of %u bytes: %s, want %s", kSha1[i].n, hex, kSha1[i].digest);
    }

    // RFC 2202 HMAC-SHA1 test cases 1, 2 and 6 (the last has a key longer than a block).
    {
        uint8_t k1[20]; memset(k1, 0x0b, 20);
        totp::hmac_sha1(k1, 20, (const uint8_t *)"Hi There", 8, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "b617318655057264e28bc0b6fb378c8ef146be00"), "hmac RFC 2202 #1 (got %s)", hex);
        totp::hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "hmac RFC 2202 #2 (got %s)", hex);
        uint8_t k6[80]; memset(k6, 0xaa, 80);
        const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
        totp::hmac_sha1(k6, 80, (const uint8_t *)m6, (unsigned)strlen(m6), d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, "aa4ae5e15272d00e95705637ce8a3b55ed402112"), "hmac RFC 2202 #6 (got %s)", hex);
    }
    for (unsigned i = 0; i < HMAC_N; i++) {
        uint8_t key[128], msg[128];
        unsigned kn = from_hex(kHmac[i].key_hex, key, sizeof(key)), mn = from_hex(kHmac[i].msg_hex, msg, sizeof(msg));
        totp::hmac_sha1(key, kn, msg, mn, d); to_hex(d, 20, hex);
        CHECK(!strcmp(hex, kHmac[i].digest), "hmac case %u: %s, want %s", i, hex, kHmac[i].digest);
    }

    // RFC 4648 base32, and what the MicroPython decoder did with the rest.
    uint8_t b[64];
    unsigned n = totp::b32_decode("MZXW6YTBOI======", b, sizeof(b));
    CHECK(n == 6 && !memcmp(b, "foobar", 6), "base32 of foobar");
    for (unsigned i = 0; i < B32_N; i++) {
        n = totp::b32_decode(kB32[i].text, b, sizeof(b)); to_hex(b, n, hex);
        CHECK(n == kB32[i].n && !strcmp(hex, kB32[i].hex), "base32 '%s': %s, want %s", kB32[i].text, hex, kB32[i].hex);
    }

    // RFC 6238 appendix B, SHA-1, the 8-digit codes.
    const uint8_t *k = (const uint8_t *)"12345678901234567890";
    char c8[12];
    static const struct { uint32_t t; const char *code; } rfc[] = {
        { 59, "94287082" }, { 1111111109u, "07081804" }, { 1111111111u, "14050471" },
        { 1234567890u, "89005924" }, { 2000000000u, "69279037" } };
    for (auto &r : rfc) {
        totp::code(k, 20, r.t, 30, 8, c8);
        CHECK(!strcmp(c8, r.code), "RFC 6238 at %u: %s, want %s", r.t, c8, r.code);
    }
    for (unsigned i = 0; i < TOTP_N; i++) {
        char c6[8];
        bool ok = totp::code_b32(kTotp[i].secret, kTotp[i].t, c6);
        CHECK(ok && !strcmp(c6, kTotp[i].code), "totp %s at %u: %s, want %s", kTotp[i].secret, kTotp[i].t, c6, kTotp[i].code);
    }
    char c6[8];
    CHECK(!totp::code_b32("!!!", 59, c6), "a secret that decodes to nothing gives no code");

    // The calendar, against a date the clock tests already know.
    CHECK(totp::unix_from_civil(1970, 1, 1, 0, 0, 0) == 0u, "the epoch is zero");
    CHECK(totp::unix_from_civil(2009, 2, 13, 23, 31, 30) == 1234567890u, "2009-02-13 23:31:30 is 1234567890");
    CHECK(totp::unix_from_civil(2026, 6, 30, 12, 20, 0) == 1782822000u, "2026-06-30 12:20:00 is 1782822000");
    CHECK(totp::unix_from_civil(2000, 3, 1, 0, 0, 0) == 951868800u, "a day after a leap February");
}

// --- GPS ---------------------------------------------------------------------------------------

static void nmea(char *out, const char *body) {
    unsigned x = 0;
    for (const char *p = body; *p; p++) x ^= (unsigned char)*p;
    sprintf(out, "$%s*%02X", body, x);
}

static void test_gps(void) {
    using namespace nova;
    char sent[160];

    // The sentences from the NMEA 0183 documentation's worked examples (the checksums are theirs).
    const char *gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    const char *rmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    CHECK(gps::checksum_ok(gga) && gps::checksum_ok(rmc), "the documented sentences have the documented checksums");
    CHECK(!gps::checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48"), "a wrong checksum is refused");
    CHECK(!gps::checksum_ok("GPGGA,1*00") && !gps::checksum_ok("$GPGGA,1") && !gps::checksum_ok("$GPGGA,1*0"), "a sentence with no frame is refused");

    gps::Fix f;
    memset(&f, 0, sizeof(f));
    CHECK(gps::consume(&f, gga), "GGA is understood");
    CHECK(f.have && f.fixed, "and says there is a fix");
    CHECK(f.lat_e5 == 4811730 && f.lon_e5 == 1151667, "at 48.11730, 11.51667 (got %d, %d)", (int)f.lat_e5, (int)f.lon_e5);
    CHECK(f.alt_dm == 5454 && f.sats_used == 8 && f.hdop_x10 == 9, "545.4 m, 8 satellites, HDOP 0.9 (got %d, %d, %d)", (int)f.alt_dm, f.sats_used, f.hdop_x10);
    CHECK(f.has_time && f.hh == 12 && f.mm == 35 && f.ss == 19, "at 12:35:19 UTC");
    CHECK(!strcmp(f.last, "GGA"), "and remembers what it last read");

    CHECK(gps::consume(&f, rmc), "RMC is understood");
    CHECK(f.speed_dkmh == 415, "22.4 knots is 41.5 km/h (got %d)", (int)f.speed_dkmh);
    CHECK(f.has_date && f.dd == 23 && f.mo == 3 && f.yy == 94, "on 23/03/94");

    // A receiver with no fix says so in the same sentence, and keeps what it knew.
    nmea(sent, "GPGGA,123520,,,,,0,00,99.99,,,,,,");
    CHECK(gps::consume(&f, sent) && !f.fixed, "a GGA with fix quality 0 is not a fix");
    nmea(sent, "GPRMC,123520,V,,,,,,,230394,,,N");
    CHECK(gps::consume(&f, sent) && !f.fixed, "an RMC with status V is not a fix");

    // South and west are negative.
    nmea(sent, "GPGGA,010203,3745.5000,S,12218.9988,W,2,09,1.1,12.0,M,0.0,M,,");
    memset(&f, 0, sizeof(f));
    CHECK(gps::consume(&f, sent) && f.lat_e5 == -3775833 && f.lon_e5 == -12231665, "south and west are negative (got %d, %d)", (int)f.lat_e5, (int)f.lon_e5);

    // Satellites in view, and a sentence that is only proof of life.
    nmea(sent, "GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00");
    CHECK(gps::consume(&f, sent) && f.sats_view == 11, "GSV gives satellites in view");
    nmea(sent, "GPVTG,054.7,T,034.4,M,005.5,N,010.2,K");
    CHECK(!gps::consume(&f, sent) && f.have, "a sentence of a kind it does not read still counts as a receiver talking");
    CHECK(!gps::consume(&f, "$GPGGA,garbage*00"), "and noise changes nothing");

    // Coordinates, against the MicroPython conversion.
    int bad = 0;
    for (unsigned i = 0; i < GPS_N; i++) {
        int32_t e5;
        char out[16];
        if (!gps::coord_e5(kGps[i].v, kGps[i].hemi, &e5)) { bad++; printf("    gps coord '%s' refused\n", kGps[i].v); continue; }
        gps::format_deg(e5, out, sizeof(out));
        if (strcmp(out, kGps[i].expect)) { if (bad++ < 5) printf("    gps '%s' %s: got %s, want %s\n", kGps[i].v, kGps[i].hemi, out, kGps[i].expect); }
    }
    CHECK(bad == 0, "%d of %u coordinates differ from the MicroPython conversion", bad, (unsigned)GPS_N);
    int32_t e;
    CHECK(!gps::coord_e5("", "N", &e) && !gps::coord_e5("48", "N", &e) && !gps::coord_e5("4860.000", "N", &e) && !gps::coord_e5("48a7.038", "N", &e),
          "things that are not coordinates are refused (a minute count of 60 included)");

    char out[16];
    gps::format_deg(-5, out, sizeof(out));
    CHECK(!strcmp(out, "-0.00005"), "a small negative keeps its sign (got %s)", out);
    gps::format_tenths(5454, out, sizeof(out));
    CHECK(!strcmp(out, "545.4"), "tenths print with one decimal");
    gps::format_tenths(-12, out, sizeof(out));
    CHECK(!strcmp(out, "-1.2"), "and a sign");
}

static void test_helpers(void) {
    Buf b; buf_init(&b);
    codec::put_fixed6(sink_of(&b), 330000);
    codec::put_chr(sink_of(&b), ' ');
    codec::put_fixed6(sink_of(&b), 5);
    codec::put_chr(sink_of(&b), ' ');
    codec::put_i32(sink_of(&b), -42);
    codec::put_chr(sink_of(&b), ' ');
    codec::put_u32(sink_of(&b), 0);
    CHECK(!strcmp(b.b, "0.330000 0.000005 -42 0"), "number formatting (got '%s')", b.b);
    int32_t v;
    CHECK(codec::parse_int("-12", "-12" + 3, &v) && v == -12, "parse_int reads a negative");
    CHECK(!codec::parse_int("12a", "12a" + 3, &v), "parse_int refuses trailing junk");
    uint64_t h;
    CHECK(codec::parse_hex("0xFF", "0xFF" + 4, &h) && h == 255, "parse_hex takes a 0x prefix");
    CHECK(!codec::parse_hex("xyz", "xyz" + 3, &h), "parse_hex refuses a non-hex digit");
}

// Bytes off a UART into lines: CR, LF and CRLF all end one, an empty line is nothing, a line too long
// to be NMEA is dropped whole, and a sentence may arrive in any number of pieces.
static void test_gps_lines(void) {
    gps::LineBuf lb;
    gps::line_reset(&lb);
    const char *ln = nullptr;
    const char *stream = "$GPGGA,1*00\r\n$GPRMC,2*00\n\n\r$GPGSV,3*00\r";
    const char *want[] = { "$GPGGA,1*00", "$GPRMC,2*00", "$GPGSV,3*00" };
    unsigned got = 0;
    for (const char *p = stream; *p; p++)
        if (gps::line_push(&lb, *p, &ln)) {
            CHECK(got < 3 && !strcmp(ln, want[got]), "line %u: got '%s'", got, ln);
            got++;
        }
    CHECK(got == 3, "three lines out of CRLF, LF LF CR and a bare CR (got %u)", got);

    // The same bytes one at a time with a reset between, and then in the middle of a line.
    gps::line_reset(&lb);
    got = 0;
    for (const char *p = "$ABC"; *p; p++) CHECK(!gps::line_push(&lb, *p, &ln), "no line before the newline");
    CHECK(gps::line_push(&lb, '\n', &ln) && !strcmp(ln, "$ABC"), "completed by the newline");

    // 200 bytes with no newline: dropped, and the line after the next newline is clean.
    gps::line_reset(&lb);
    bool spurious = false;
    for (int i = 0; i < 200; i++) if (gps::line_push(&lb, 'x', &ln)) spurious = true;
    CHECK(!spurious, "a line too long is not handed over");
    gps::line_push(&lb, '\n', &ln);
    bool clean = false;
    for (const char *p = "$OK\n"; *p; p++) if (gps::line_push(&lb, *p, &ln)) clean = !strcmp(ln, "$OK");
    CHECK(clean, "and the next line is read whole");
}

// --- WiGLE ---------------------------------------------------------------------------------

static void test_wigle(void) {
    for (unsigned i = 0; i < WIGLE_SEC_N; i++)
        CHECK(!strcmp(wigle::sec_text(kWigleSec[i].bits), kWigleSec[i].text),
              "security bits %u: got '%s', want '%s'", kWigleSec[i].bits,
              wigle::sec_text(kWigleSec[i].bits), kWigleSec[i].text);

    for (unsigned i = 0; i < WIGLE_HEADER_N; i++) {
        Buf b; buf_init(&b);
        CHECK(wigle::put_header(sink_of(&b), kWigleHeader[i].model), "header %u written", i);
        CHECK(!strcmp(b.b, kWigleHeader[i].text), "header %u matches the oracle (got '%s')", i, b.b);
    }
    for (unsigned i = 0; i < WIGLE_FIELD_N; i++) {
        Buf b; buf_init(&b);
        CHECK(wigle::put_field(sink_of(&b), kWigleField[i].in), "field %u written", i);
        CHECK(!strcmp(b.b, kWigleField[i].out), "field '%s': got '%s', want '%s'", kWigleField[i].in, b.b,
              kWigleField[i].out);
    }
    for (unsigned i = 0; i < WIGLE_ROW_N; i++) {
        const WigleRowCase &c = kWigleRow[i];
        wigle::Row r = {};
        r.bssid = c.bssid; r.ssid = c.ssid; r.sec = c.sec; r.time = c.time;
        r.channel = c.channel; r.rssi = c.rssi;
        r.has_pos = c.pos != 0; r.lat_e5 = c.lat_e5; r.lon_e5 = c.lon_e5;
        r.has_alt = c.has_alt != 0; r.alt_dm = c.alt_dm;
        Buf b; buf_init(&b);
        CHECK(wigle::put_row(sink_of(&b), r), "row %u written", i);
        CHECK(!strcmp(b.b, c.row), "row %u: got '%s' want '%s'", i, b.b, c.row);
    }

    // The session: a radio is a row on the pass it first appears and never again.
    wigle::Seen seen;
    wigle::seen_reset(&seen);
    unsigned total = 0;
    for (unsigned p = 0; p < WIGLE_PASS_N; p++) {
        for (unsigned k = 0; k < kWiglePass[p].n; k++) {
            uint8_t mac[6] = { 2, 0, 0, 0, 0, kWiglePass[p].id[k] };
            bool fresh = !wigle::seen_note(&seen, mac);
            CHECK(fresh == (kWiglePass[p].fresh[k] != 0), "pass %u entry %u: fresh=%d, oracle says %d", p, k,
                  (int)fresh, (int)kWiglePass[p].fresh[k]);
            if (fresh) total++;
        }
    }
    CHECK(total == kWigleSessionTotal, "%u unique radios, oracle says %u", total, kWigleSessionTotal);

    // What the oracle could not be asked, because its set never ran out: a full table forgets the
    // OLDEST radio, not the newest, so a long drive keeps logging new ones.
    wigle::seen_reset(&seen);
    for (unsigned i = 0; i < WIGLE_SEEN_MAX + 10; i++) {
        uint8_t mac[6] = { 0x0a, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };
        CHECK(!wigle::seen_note(&seen, mac), "radio %u is new", i);
    }
    CHECK(seen.n == WIGLE_SEEN_MAX, "the table stays at its bound");
    {
        uint8_t newest[6] = { 0x0a, 0, 0, 0, (uint8_t)((WIGLE_SEEN_MAX + 9) >> 8), (uint8_t)(WIGLE_SEEN_MAX + 9) };
        uint8_t kept[6]   = { 0x0a, 0, 0, 0, 0, 10 };      // the first one not yet overwritten
        CHECK(wigle::seen_note(&seen, newest), "the newest radio is still remembered");
        CHECK(wigle::seen_note(&seen, kept), "one that was not the oldest is still remembered");
        // The ten overwritten were the ten oldest, one after another - not the same slot ten times.
        // Asked after the two above, because asking about a forgotten radio remembers it again.
        for (unsigned i = 0; i < 10; i++) {
            uint8_t gone[6] = { 0x0a, 0, 0, 0, 0, (uint8_t)i };
            CHECK(!wigle::seen_note(&seen, gone), "radio %u was forgotten, and so counts as new again", i);
        }
    }

    // A sink that fills must stop the writer and say so, at every length.
    {
        const uint8_t mac[6] = { 1, 2, 3, 4, 5, 6 };
        wigle::Row r = {};
        r.bssid = mac; r.ssid = "Full,House"; r.sec = 4; r.time = "2026-10-30 18:04:05";
        r.channel = 6; r.rssi = -60; r.has_pos = true; r.lat_e5 = 4807038; r.lon_e5 = -1131000;
        r.has_alt = true; r.alt_dm = 5454;
        Buf whole; buf_init(&whole);
        wigle::put_row(sink_of(&whole), r);
        bool all_stop = true;
        for (unsigned cap = 1; cap <= whole.n; cap++) {
            Buf b; buf_init(&b, cap);
            if (wigle::put_row(sink_of(&b), r)) all_stop = false;
        }
        CHECK(all_stop, "a row that does not fit is refused at every shorter length");
    }
}

int main() {
    test_helpers();
    test_oui();
    test_totp();
    test_gps();
    test_gps_lines();
    test_wigle();
    test_sub_encode();
    test_sub_parse();
    test_sub_write();
    test_ir_encode();
    test_ir_parse();
    test_ir_write();
    test_nfc();
    test_nfc_dump();
    printf("  novacodec_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
