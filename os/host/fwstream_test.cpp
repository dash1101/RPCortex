// The deflated CYW43 firmware, read back the way the patched driver reads it,
// against the SDK's own copy of the same firmware.
//
// run_all.sh makes the stream with the generator the build uses
// (tools_cyw43fw.py --pad 4) and compiles uzlib as C; the reference is the SDK
// header itself, included below exactly as the driver would include it. So this
// covers the generator's parsing of that header as well as the reader.
//
// What the driver does with the stream, and so what is exercised here:
//   - the validity check reads the last 800 bytes of the firmware (kept apart,
//     uncompressed);
//   - the download reads CYW43_WRITE_BYTES_PAD(CYW43_WIFI_FW_LEN) bytes in
//     CYW43_BUS_MAX_BLOCK_SIZE pieces, which on the SPI bus is 64, the last 40;
//   - the CLM is read in one piece of CYW43_CLM_LEN, once to check it before
//     the download and once to send it.
//
// THE PROPERTY THAT MATTERS is the one the corruption sweep holds the reader to:
// whatever a damaged stream does, no byte is ever handed out that differs from
// the original. Being refused is necessary but not enough; a reader that
// refuses at the end, after handing out garbage, would have sent it to the chip.
#include "../core/fwstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

// The reference, as the SDK ships it. Its two lengths are kept before the
// generated header defines the same names, so the two can be compared.
#define CYW43_RESOURCE_ATTRIBUTE
#include "wb43439A0_7_95_49_00_combined.h"
static const uint32_t kSdkFwLen  = CYW43_WIFI_FW_LEN;
static const uint32_t kSdkClmLen = CYW43_CLM_LEN;
#undef CYW43_WIFI_FW_LEN
#undef CYW43_CLM_LEN
#include "cyw43_fw_data.h"

static const uint8_t *const ref = wb43439A0_7_95_49_00_combined;
static const uint32_t kClmOff = (CYW43_WIFI_FW_LEN + 511u) & ~511u;   // CYW43_CLM_ADDR
static const uint32_t kBusBlock = 64;                                 // CYW43_BUS_MAX_BLOCK_SIZE, SPI

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

static const FwImage kWifi = { "Wi-Fi firmware", cyw43_fw_wifi_z, CYW43_FW_WIFI_ZLEN,
                               CYW43_FW_WIFI_LEN, cyw43_fw_wifi_crc };
static const FwImage kClm  = { "Wi-Fi CLM", cyw43_fw_clm_z, CYW43_FW_CLM_ZLEN,
                               CYW43_CLM_LEN, cyw43_fw_clm_crc };

// What one pass through a stream did.
struct Pass {
    uint32_t handed;     // bytes handed out before the first refusal (or all)
    uint32_t wrong;      // of those, how many differ from `expect`
    int      err;        // fwstream_close's verdict
    uint32_t last;       // size of the last read that succeeded
};

// Read img in blocks of `block` (the last one short), as the driver's loop
// does, comparing everything handed out with `expect`.
static Pass run(const FwImage *img, uint32_t block, const uint8_t *expect) {
    Pass p = {0, 0, -1, 0};
    FwStream *s = fwstream_open(img);
    if (!s) return p;
    for (uint32_t off = 0; off < img->len; off += block) {
        uint32_t sz = block;
        if (off + sz > img->len) sz = img->len - off;
        const uint8_t *b = fwstream_read(s, sz);
        if (!b) break;
        for (uint32_t i = 0; i < sz; i++) p.wrong += b[i] != expect[off + i];
        p.handed += sz;
        p.last = sz;
    }
    p.err = fwstream_close(s, nullptr);
    return p;
}

// cyw43_check_valid_chipset_firmware, transcribed from cyw43_ll.c: it looks for
// "Version: " in the trailer at the end of the firmware. The driver calls it on
// the stored tail (len 800); here it is run on both, which must agree.
static bool driver_validity_check(const uint8_t *source, uint32_t len) {
    uint32_t fw_end = 800;
    const uint8_t *b = source + len - fw_end;
    fw_end -= 16;
    uint32_t trail_len = b[fw_end - 2] | b[fw_end - 1] << 8;
    if (trail_len < 500 && b[fw_end - 3] == '\0') {
        for (int i = 80; i < (int)trail_len; ++i)
            if (strncmp((const char *)&b[fw_end - 3 - i], "Version: ", 9) == 0) return true;
    }
    return false;
}

// --- a hand-built deflate stream, for the edges zlib will not produce ---------
struct Bits {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) {            // a value, least significant bit first
        for (int i = 0; i < bits; i++) {
            acc |= ((v >> i) & 1u) << n;
            if (++n == 8) { out.push_back((uint8_t)acc); acc = 0; n = 0; }
        }
    }
    void code(uint32_t c, int bits) {           // a Huffman code, most significant bit first
        for (int i = bits - 1; i >= 0; i--) put((c >> i) & 1u, 1);
    }
    void flush() { if (n) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
    // Fixed Huffman codes (RFC 1951, 3.2.6).
    void lit(uint8_t b) { if (b < 144) code(0x30 + b, 8); else code(0x190 + b - 144, 9); }
    void eob() { code(0, 7); }
    void match(uint32_t len, uint32_t dist) {   // len 3 to 10: symbols 257-264, no extra bits
        code(len - 2, 7);
        static const uint16_t base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        int c = 29;
        while (base[c] > dist) c--;
        code((uint32_t)c, 5);
        put(dist - base[c], c < 4 ? 0 : c / 2 - 1);
    }
};

// An image of `data`, its CRCs computed here.
struct Built {
    std::vector<uint8_t> z, plain;
    std::vector<uint32_t> crc;
    FwImage img;
    void seal() {
        crc.clear();
        for (size_t i = 0; i < plain.size(); i += FWSTREAM_WINDOW) {
            size_t n = plain.size() - i < FWSTREAM_WINDOW ? plain.size() - i : FWSTREAM_WINDOW;
            crc.push_back(fwstream_crc32(plain.data() + i, (uint32_t)n));
        }
        img = { "built", z.data(), (uint32_t)z.size(), (uint32_t)plain.size(), crc.data() };
    }
};

// 4096 literals, then one 3-byte match `dist` back: the window is 2048, so 2048
// is the furthest a stream may reach.
static Built far_match(uint32_t dist) {
    Built b;
    Bits w;
    w.put(1, 1); w.put(1, 2);                   // final block, fixed Huffman
    uint32_t seed = 7;
    for (int i = 0; i < 4096; i++) {
        seed = seed * 1103515245u + 12345u;
        uint8_t c = (uint8_t)(seed >> 16);
        b.plain.push_back(c);
        w.lit(c);
    }
    for (int i = 0; i < 3; i++) b.plain.push_back(b.plain[b.plain.size() - dist]);
    w.match(3, dist);
    w.eob();
    w.flush();
    b.z = w.out;
    b.seal();
    return b;
}

int main() {
    printf("fwstream:\n");

    // --- the generated store against the SDK's array -------------------------
    ck(kSdkFwLen == CYW43_WIFI_FW_LEN && kSdkClmLen == CYW43_CLM_LEN,
       "the generated lengths are the SDK's");
    ck(sizeof(wb43439A0_7_95_49_00_combined) == kClmOff + CYW43_CLM_LEN,
       "the SDK array is the firmware, then the CLM at the next 512");
    ck(CYW43_FW_WIFI_LEN == ((CYW43_WIFI_FW_LEN + 3u) & ~3u),
       "the stream is the firmware padded as the SPI bus sends it");
    ck(CYW43_FW_WINDOW == FWSTREAM_WINDOW, "generator and reader agree on the window");
    ck(FWSTREAM_WINDOW % kBusBlock == 0, "a bus block never spans two windows");
    ck(sizeof(cyw43_fw_wifi_crc) / 4 == (CYW43_FW_WIFI_LEN + FWSTREAM_WINDOW - 1) / FWSTREAM_WINDOW,
       "one CRC per window");

    // --- the validity check, on the stored tail -----------------------------
    ck(memcmp(cyw43_fw_tail, ref + CYW43_WIFI_FW_LEN - CYW43_FW_TAIL_LEN, CYW43_FW_TAIL_LEN) == 0,
       "the stored tail is the end of the firmware");
    ck(driver_validity_check(ref, CYW43_WIFI_FW_LEN), "the original passes the driver's check");
    ck(driver_validity_check(cyw43_fw_tail, CYW43_FW_TAIL_LEN), "and so does the tail on its own");

    // --- the download, in the driver's own block size -------------------------
    Pass p = run(&kWifi, kBusBlock, ref);
    ck(p.err == FWS_OK, "the firmware inflates and every window checks");
    ck(p.handed == CYW43_FW_WIFI_LEN && p.wrong == 0, "byte for byte the SDK's firmware");
    ck(p.last == CYW43_FW_WIFI_LEN % kBusBlock, "the last block is the short one (40)");
    // The padding is what the driver sent before too: the array's own bytes.
    ck(memcmp(ref + CYW43_WIFI_FW_LEN, "\0\0\0", CYW43_FW_WIFI_LEN - CYW43_WIFI_FW_LEN) == 0,
       "the padding sent is the array's (zeros)");

    // Other block sizes that divide the window behave the same.
    static const uint32_t sizes[] = { 4, 512, 1024, 2048 };
    for (uint32_t bs : sizes) {
        Pass q = run(&kWifi, bs, ref);
        char what[64]; snprintf(what, sizeof what, "blocks of %u read the same firmware", (unsigned)bs);
        ck(q.err == FWS_OK && q.handed == CYW43_FW_WIFI_LEN && q.wrong == 0, what);
    }
    // One that does not is refused at the first block that would span two
    // windows, not handed a short block.
    {
        Pass q = run(&kWifi, 100, ref);
        ck(q.err == FWS_BAD_READ && q.wrong == 0 && q.handed == 2000,
           "a block spanning two windows is refused, after 2000 good bytes");
        Pass r = run(&kWifi, 4096, ref);
        ck(r.err == FWS_BAD_READ && r.handed == 0, "a block larger than the window is refused");
    }

    // --- the CLM ---------------------------------------------------------------
    {
        Pass q = run(&kClm, CYW43_CLM_LEN, ref + kClmOff);
        ck(q.err == FWS_OK && q.handed == CYW43_CLM_LEN && q.wrong == 0,
           "the CLM, in one read, is the SDK's");
        Pass r = run(&kClm, kBusBlock, ref + kClmOff);
        ck(r.err == FWS_OK && r.wrong == 0, "and in bus blocks");
        ck(CYW43_CLM_LEN <= FWSTREAM_WINDOW, "the CLM fits one window");
    }

    // --- misuse --------------------------------------------------------------
    {
        FwStream *s = fwstream_open(&kWifi);
        ck(s && fwstream_read(s, 64), "a first read");
        uint32_t at = 0;
        ck(fwstream_close(s, &at) == FWS_UNREAD && at == 64, "closing early is not success");
        s = fwstream_open(&kClm);
        ck(!fwstream_read(s, 0), "a read of nothing is refused");
        ck(!fwstream_read(s, 64), "and the refusal sticks");
        ck(fwstream_close(s, nullptr) == FWS_BAD_READ, "and is what close reports");
        s = fwstream_open(&kClm);
        ck(!fwstream_read(s, CYW43_CLM_LEN + 1), "a read past the end is refused");
        fwstream_close(s, nullptr);
        ck(fwstream_close(nullptr, nullptr) != FWS_OK, "closing nothing is not success");
    }

    // --- damage: nothing wrong is ever handed out ---------------------------
    //
    // A bit flipped at a spread of places through the stream, densest in the
    // first few hundred bytes where the first block's Huffman tables are. A
    // flip can, rarely, leave the output unchanged (a padding bit after the
    // final code), so the hard requirement is: refused, or every byte right.
    {
        std::vector<uint8_t> z(cyw43_fw_wifi_z, cyw43_fw_wifi_z + CYW43_FW_WIFI_ZLEN);
        FwImage img = kWifi;
        img.z = z.data();
        std::vector<uint32_t> at;
        for (uint32_t i = 0; i < 300; i += 3) at.push_back(i);
        for (uint32_t i = 300; i < z.size(); i += (uint32_t)z.size() / 300) at.push_back(i);
        at.push_back((uint32_t)z.size() - 1);
        uint32_t refused = 0, wrong = 0, accepted_wrong = 0;
        for (size_t k = 0; k < at.size(); k++) {
            uint8_t bit = (uint8_t)(1u << (k % 8));
            z[at[k]] ^= bit;
            Pass q = run(&img, kBusBlock, ref);
            z[at[k]] ^= bit;
            wrong += q.wrong;
            if (q.err != FWS_OK) refused++;
            else if (q.handed != CYW43_FW_WIFI_LEN) accepted_wrong++;
        }
        char what[96];
        snprintf(what, sizeof what, "%u flips: no wrong byte handed out", (unsigned)at.size());
        ck(wrong == 0, what);
        ck(accepted_wrong == 0, "nothing short is accepted");
        snprintf(what, sizeof what, "nearly all refused (%u of %u)", (unsigned)refused, (unsigned)at.size());
        ck(refused + 2 >= at.size(), what);
        printf("  bit flips: %u of %u refused, 0 wrong bytes out\n", (unsigned)refused, (unsigned)at.size());
    }
    {
        // And at random: one to four bytes replaced, anywhere. This is what
        // found uzlib indexing lookup_table[-3] on a damaged stream (fixed in
        // uzlib/tinflate.c, marked there). RPC_FUZZ_ITERS for a longer run.
        uint32_t iters = 150;
        if (const char *e = getenv("RPC_FUZZ_ITERS")) iters = (uint32_t)strtoul(e, nullptr, 10);
        std::vector<uint8_t> z(cyw43_fw_wifi_z, cyw43_fw_wifi_z + CYW43_FW_WIFI_ZLEN);
        FwImage img = kWifi;
        img.z = z.data();
        uint32_t seed = 12345, wrong = 0, accepted_wrong = 0;
        auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
        for (uint32_t it = 0; it < iters; it++) {
            uint32_t pos[4]; uint8_t was[4];
            uint32_t n = 1 + rnd() % 4;
            for (uint32_t k = 0; k < n; k++) {
                // Half the time in the first 2 KB, where the first block's
                // trees are; otherwise anywhere.
                pos[k] = rnd() % (rnd() & 1 ? 2048u : (uint32_t)z.size());
                was[k] = z[pos[k]];
                z[pos[k]] = (uint8_t)rnd();
            }
            Pass q = run(&img, kBusBlock, ref);
            for (uint32_t k = n; k-- > 0;) z[pos[k]] = was[k];
            wrong += q.wrong;
            if (q.err == FWS_OK && q.handed != CYW43_FW_WIFI_LEN) accepted_wrong++;
        }
        char what[96];
        snprintf(what, sizeof what, "%u random damages: no wrong byte handed out", (unsigned)iters);
        ck(wrong == 0 && accepted_wrong == 0, what);
    }
    {
        // A wrong CRC on one window: that window and nothing after it.
        std::vector<uint32_t> crc(cyw43_fw_wifi_crc, cyw43_fw_wifi_crc + sizeof(cyw43_fw_wifi_crc) / 4);
        crc[50] ^= 1;
        FwImage img = kWifi;
        img.crc = crc.data();
        Pass q = run(&img, kBusBlock, ref);
        ck(q.err == FWS_CRC && q.handed == 50u * FWSTREAM_WINDOW && q.wrong == 0,
           "a bad CRC stops the stream at its own window");
    }
    {
        // Cut short, at the start, the middle and the last byte.
        const uint32_t cuts[] = { 1, CYW43_FW_WIFI_ZLEN / 2, CYW43_FW_WIFI_ZLEN - 1 };
        for (uint32_t cut : cuts) {
            FwImage img = kWifi;
            img.z_len = cut;
            Pass q = run(&img, kBusBlock, ref);
            char what[64]; snprintf(what, sizeof what, "a stream cut to %u bytes is refused", (unsigned)cut);
            ck(q.err != FWS_OK && q.wrong == 0, what);
        }
    }
    {
        // Something after the end. The last window is held back as well: it is
        // not handed out until the stream is known to end where the image does.
        std::vector<uint8_t> z(cyw43_fw_wifi_z, cyw43_fw_wifi_z + CYW43_FW_WIFI_ZLEN);
        z.push_back(0);
        FwImage img = kWifi;
        img.z = z.data();
        img.z_len = (uint32_t)z.size();
        Pass q = run(&img, kBusBlock, ref);
        ck(q.err == FWS_TRAILING && q.wrong == 0, "a byte after the end is refused");
        ck(q.handed == CYW43_FW_WIFI_LEN / FWSTREAM_WINDOW * FWSTREAM_WINDOW,
           "and the last window is never handed out");
        // A length that disagrees with the stream, either way. (The last
        // window's CRC catches both before the end does.)
        FwImage shorter = kWifi;
        shorter.len -= 4;
        Pass s = run(&shorter, 4, ref);
        ck(s.err != FWS_OK && s.wrong == 0, "a stream longer than the image is refused");
        FwImage longer = kWifi;
        longer.len += 4;
        Pass l = run(&longer, 4, ref);
        ck(l.err != FWS_OK && l.wrong == 0, "a stream shorter than the image is refused");
    }

    // --- the window ----------------------------------------------------------
    {
        Built in = far_match(2048), out = far_match(2049);
        Pass q = run(&in.img, kBusBlock, in.plain.data());
        ck(q.err == FWS_OK && q.handed == 4099 && q.wrong == 0, "a match 2048 back is inside the window");
        Pass r = run(&out.img, kBusBlock, out.plain.data());
        ck(r.err == FWS_CORRUPT && r.handed == 4096 && r.wrong == 0,
           "one 2049 back is refused, the two windows before it intact");
    }
    {
        // A stored block of zeros, cut short. uzlib reads zeros past the end of
        // its input, so the output and its CRC would both be right; only the
        // end of input says otherwise.
        Built b;
        Bits w;
        w.put(1, 1); w.put(0, 2); w.flush();
        w.put(100, 16); w.put(0xffff ^ 100, 16);
        b.z = w.out;
        b.z.insert(b.z.end(), 60, 0);           // 60 of the 100 bytes
        b.plain.assign(100, 0);
        b.seal();
        ck(run(&b.img, 4, b.plain.data()).err == FWS_SHORT, "a cut stored block is refused");
        b.z.insert(b.z.end(), 40, 0);
        b.seal();
        ck(run(&b.img, 4, b.plain.data()).err == FWS_OK, "and whole, accepted");
    }
    {
        // The same across two windows, cut inside the first. The first window
        // comes out as zeros with a matching CRC, so only the end of input can
        // stop it there rather than one window later.
        Built b;
        Bits w;
        w.put(1, 1); w.put(0, 2); w.flush();
        w.put(4096, 16); w.put(0xffff ^ 4096, 16);
        b.z = w.out;
        b.z.insert(b.z.end(), 1000, 0);
        b.plain.assign(4096, 0);
        b.seal();
        Pass q = run(&b.img, 64, b.plain.data());
        ck(q.err == FWS_SHORT && q.handed == 0,
           "nothing inflated past the end of the input is handed out");
    }
    {
        // A stream that goes on past the image inside a back-reference, with
        // its end-of-block in a byte already read: the input is used up and the
        // window's CRC is right, so only asking for one more byte shows that
        // the stream has more to give. The bits are counted so that this holds:
        // 3 header + two 9-bit literals + 100 8-bit ones + 7 + 5 for the match
        // leaves the 7-bit end-of-block in the last byte's remaining 7 bits.
        Built b;
        Bits w;
        w.put(1, 1); w.put(1, 2);
        std::vector<uint8_t> all;
        for (int i = 0; i < 2; i++) { w.lit(200); all.push_back(200); }
        for (int i = 0; i < 100; i++) { w.lit((uint8_t)('a' + i % 26)); all.push_back((uint8_t)('a' + i % 26)); }
        w.match(10, 1);
        for (int i = 0; i < 10; i++) all.push_back(all.back());
        w.eob();
        w.flush();
        b.z = w.out;
        b.plain.assign(all.begin(), all.end() - 5);   // the image stops 5 bytes into the match
        b.seal();
        Pass q = run(&b.img, 1, b.plain.data());
        ck(q.err == FWS_TRAILING && q.handed == 0, "a stream with more after the image is refused");
        b.plain = all;
        b.seal();
        ck(run(&b.img, 1, b.plain.data()).err == FWS_OK, "and with the image its full length, accepted");
    }

    // --- what it costs -------------------------------------------------------
    {
        clock_t t0 = clock();
        Pass q = run(&kWifi, kBusBlock, ref);
        double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
        ck(q.err == FWS_OK, "a timed pass");
        printf("  stored %u + %u + %u CRC + %u tail bytes for %u; reader %u bytes of heap here;"
               " one pass %.1f ms on this host, sanitizers on\n",
               (unsigned)CYW43_FW_WIFI_ZLEN, (unsigned)CYW43_FW_CLM_ZLEN,
               (unsigned)(sizeof(cyw43_fw_wifi_crc) + sizeof(cyw43_fw_clm_crc)),
               (unsigned)CYW43_FW_TAIL_LEN, (unsigned)sizeof(wb43439A0_7_95_49_00_combined),
               (unsigned)fwstream_footprint(), ms);
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
