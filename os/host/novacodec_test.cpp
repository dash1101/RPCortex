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
static bool page_get(void *ctx, unsigned i, uint8_t *out) {
    (void)ctx;
    if (i == 3) return false;
    for (unsigned k = 0; k < 4; k++) out[k] = (uint8_t)(i + k);
    return true;
}
static bool block_get(void *, unsigned i, uint8_t *out) {
    if (i == 5) return false;
    for (unsigned k = 0; k < 16; k++) out[k] = (uint8_t)(i + k);
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

int main() {
    test_helpers();
    test_oui();
    test_sub_encode();
    test_sub_parse();
    test_sub_write();
    test_ir_encode();
    test_ir_parse();
    test_ir_write();
    test_nfc();
    printf("  novacodec_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
