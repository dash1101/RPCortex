// Desc: Walking a selected card into a .nfc dump - see novanfcdump.h.
// File: novanfcdump.cpp
#include "novanfcdump.h"

#include <string.h>

namespace nova {
namespace nfcdump {

// A self-contained string copy, so this file does not pull novacore (and its fw_* calls) into the
// codec host test. Always terminates.
static void kind_copy(char *dst, unsigned cap, const char *src) {
    unsigned i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    if (cap) dst[i] = 0;
}

// The factory and transport keys every NFC library ships (libnfc, libfreefare, the Flipper's own
// list). Kept short so a dump does not spend minutes on keys a hobby card never uses: each miss is
// a re-select and an auth round-trip. A sector that opens to none of them is left "??".
static const uint8_t kKeys[][6] = {
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 },
    { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5 }, { 0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD },
    { 0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A }, { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
    { 0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97 }, { 0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F },
    { 0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91 }, { 0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6 },
    { 0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9 }, { 0x02, 0x97, 0x92, 0x7C, 0x0F, 0x77 },
    { 0x5C, 0x8F, 0xF9, 0x99, 0x0D, 0xA2 }, { 0x75, 0xCC, 0xB5, 0x9C, 0x9B, 0xED },
};
#define N_KEYS (sizeof(kKeys) / sizeof(kKeys[0]))

// Mifare Classic geometry from the SAK. 1K is 64 blocks in 16 four-block sectors; 4K is 256
// blocks, the first 32 sectors four blocks and the last 8 sixteen; a Mini is 20 blocks in 5.
static void classic_geometry(uint8_t sak, unsigned *blocks, const char **kind) {
    if (sak == 0x18 || sak == 0x98) { *blocks = 256; *kind = "4K"; }
    else if (sak == 0x09)           { *blocks = 20;  *kind = "Mini"; }
    else                            { *blocks = 64;  *kind = "1K"; }
}
static unsigned classic_sectors(unsigned blocks) { return blocks == 256 ? 40u : blocks / 4u; }
static unsigned sector_first(unsigned sec) { return sec < 32 ? sec * 4u : 128u + (sec - 32u) * 16u; }
static unsigned sector_blocks(unsigned sec) { return sec < 32 ? 4u : 16u; }

// NTAG / Ultralight page count from the GET_VERSION storage byte (manufacturer data byte 6).
static void ntag_from_version(uint8_t storage, unsigned *pages, const char **kind) {
    switch (storage) {
        case 0x0F: *pages = 45;  *kind = "NTAG213"; return;
        case 0x11: *pages = 135; *kind = "NTAG215"; return;
        case 0x13: *pages = 231; *kind = "NTAG216"; return;
        case 0x0B: *pages = 20;  *kind = "Mifare Ultralight 11"; return;
        case 0x0E: *pages = 41;  *kind = "Mifare Ultralight 21"; return;
        default:   *pages = 135; *kind = "NTAG215"; return;   // the MicroPython default
    }
}

static bool stop(const Reader &r) { return r.cancelled && r.cancelled(r.ctx); }
static void tell(const Reader &r, unsigned done, unsigned total) { if (r.progress) r.progress(r.ctx, done, total); }

// Authenticate one block and, on success, read the blocks of its sector into the dump.
//
// A Mifare read of a sector trailer hands back zeros where Key A sits (the card never reveals it)
// and Key B only where the access bits permit. So the trailer is written with the key that opened
// the sector filled into its own slot - that key is known - and the other key's six bytes marked
// unknown rather than trusted. The access bits the card returned are kept.
static bool read_sector(const Reader &r, const Tag &t, unsigned sec, Dump *d) {
    const unsigned first = sector_first(sec);
    const unsigned nb    = sector_blocks(sec);
    const unsigned trailer = first + nb - 1;

    for (unsigned kt = 0; kt < 2; kt++) {               // Key A (0x60) then Key B (0x61)
        for (unsigned ki = 0; ki < N_KEYS; ki++) {
            if (stop(r)) return false;
            Tag again;
            if (r.select(r.ctx, &again, /*reset*/true) != 1) continue;
            uint8_t auth[12] = { (uint8_t)(0x60 + kt), (uint8_t)first };
            memcpy(auth + 2, kKeys[ki], 6);
            memcpy(auth + 8, t.uid, 4);                 // the card-side auth uses the UID's first 4
            uint8_t rx[18];
            if (r.exchange(r.ctx, auth, sizeof(auth), rx, sizeof(rx)) < 0) continue;

            // Authenticated: read every block of the sector.
            for (unsigned off = 0; off < nb; off++) {
                if (stop(r)) return false;
                const unsigned blk = first + off;
                uint8_t cmd[2] = { 0x30, (uint8_t)blk };
                uint8_t data[18];
                const int n = r.exchange(r.ctx, cmd, sizeof(cmd), data, sizeof(data));
                if (n >= 16) {
                    memcpy(d->mem + blk * 16u, data, 16);
                    d->known[blk] = 0xFFFF;
                    d->read_units++;
                    if (blk == trailer) {
                        const unsigned key_at = (kt == 0) ? 0u : 10u;
                        const unsigned other  = (kt == 0) ? 10u : 0u;
                        memcpy(d->mem + blk * 16u + key_at, kKeys[ki], 6);     // the key we know
                        d->known[blk] &= (uint16_t)~(0x3Fu << other);          // the other is not trusted
                    }
                }
                tell(r, blk + 1, d->units);
            }
            return true;
        }
    }
    // Nothing opened this sector; its blocks stay "??". Count them as visited so progress completes.
    for (unsigned off = 0; off < nb; off++) tell(r, first + off + 1, d->units);
    return false;
}

static int dump_classic(const Reader &r, const Tag &t, Dump *d) {
    unsigned blocks; const char *kind;
    classic_geometry(t.sak, &blocks, &kind);
    d->type = DUMP_CLASSIC;
    kind_copy(d->kind, sizeof(d->kind), kind);
    d->units = blocks;
    d->unit_bytes = 16;
    const unsigned nsec = classic_sectors(blocks);
    tell(r, 0, blocks);
    for (unsigned sec = 0; sec < nsec; sec++) {
        if (stop(r)) return DUMP_CANCELLED;
        read_sector(r, t, sec, d);
    }
    return DUMP_OK;
}

// Pull a 0xD5 0x41-style data exchange's bytes. The firmware's fw_nfc_exchange already stripped
// the status byte and handed back only the data, so this is the raw card reply.
static int dump_ntag(const Reader &r, const Tag &t, Dump *d) {
    d->type = DUMP_ULTRALIGHT;
    d->unit_bytes = 4;

    // GET_VERSION (0x60) is eight bytes; byte 6 is the storage size that names the chip.
    uint8_t ver[16];
    const uint8_t get_ver[1] = { 0x60 };
    const int vn = r.exchange(r.ctx, get_ver, 1, ver, sizeof(ver));
    unsigned pages; const char *kind;
    if (vn >= 8) {
        d->has_ver = true;
        memcpy(d->mifare_version, ver, 8);
        ntag_from_version(ver[6], &pages, &kind);
    } else {
        ntag_from_version(0xFF, &pages, &kind);         // unknown: the default count
    }
    kind_copy(d->kind, sizeof(d->kind), kind);
    d->units = pages;

    // READ_SIG (0x3C 0x00) is the 32-byte originality signature. Optional - older tags refuse it.
    uint8_t sig[48];
    const uint8_t read_sig[2] = { 0x3C, 0x00 };
    if (r.exchange(r.ctx, read_sig, sizeof(read_sig), sig, sizeof(sig)) >= 32) {
        d->has_sig = true;
        memcpy(d->sig, sig, 32);
    }

    // READ (0x30 page) returns four pages at once. Walk them four at a time.
    tell(r, 0, pages);
    for (unsigned p = 0; p < pages; p += 4) {
        if (stop(r)) return DUMP_CANCELLED;
        uint8_t cmd[2] = { 0x30, (uint8_t)p };
        uint8_t data[18];
        const int n = r.exchange(r.ctx, cmd, sizeof(cmd), data, sizeof(data));
        if (n < 16) break;                              // a read that fails ends the recoverable part
        for (unsigned k = 0; k < 4 && p + k < pages; k++) {
            memcpy(d->mem + (p + k) * 4u, data + k * 4u, 4);
            d->known[p + k] = 0x000F;                   // four bytes, so four bits
            d->read_units++;
        }
        tell(r, p + 4 < pages ? p + 4 : pages, pages);
    }
    return DUMP_OK;
}

int run(const Reader &r, const Tag &first, Dump *out) {
    memset(out, 0, sizeof(*out));
    out->tag = first;

    const nfc::Kind k = nfc::identify(first.sak, first.atqa, 2, 0);
    if (!strcmp(k.device_type, NFC_DT_CLASSIC))    return dump_classic(r, first, out);
    if (!strcmp(k.device_type, NFC_DT_ULTRALIGHT)) return dump_ntag(r, first, out);

    // A card whose memory this does not know how to read. Its identity is still worth saving.
    out->type = DUMP_ISO3A;
    out->units = 0;
    return DUMP_UNSUPPORTED;
}

bool unit_getter(void *dump, unsigned index, uint8_t *out, uint16_t *known) {
    const Dump *d = (const Dump *)dump;
    if (index >= d->units) return false;
    memcpy(out, d->mem + index * d->unit_bytes, d->unit_bytes);
    *known = d->known[index];
    return d->known[index] != 0;
}

}  // namespace nfcdump
}  // namespace nova
