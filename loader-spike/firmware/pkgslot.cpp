#include "pkgslot.h"
#include "api.h"     // api_symbol_count / api_abi_prefix_crc: the running firmware's
                     // ABI-table identity, recorded at commit and checked at open

#include <string.h>

// The metadata has to fit the fixed window the CRC covers. If a field is added
// past 512 bytes this stops the build rather than truncating the record and
// producing a slot that validates and describes something else.
static_assert(sizeof(PkgSlotMeta) <= PKGSLOT_META_BYTES, "PkgSlotMeta outgrew its page");
static_assert(sizeof(PkgSlotCommit) <= PKGSLOT_PROG, "commit record outgrew its page");
static_assert(PKGSLOT_META_OFF + PKGSLOT_META_BYTES <= PKGSLOT_HEADER,
              "metadata does not fit the header sector");

// CRC-32, bitwise. No table: 1 KB of lookup for something that runs once per
// install over a hundred kilobytes is the wrong trade on a device with 500 KB of
// RAM, and the difference is milliseconds against an erase that takes seconds.
uint32_t pkgslot_crc32(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

const char *pkgslot_status_str(PkgSlotStatus s) {
    switch (s) {
    case PKGSLOT_OK:         return "ok";
    case PKGSLOT_EMPTY:      return "empty";
    case PKGSLOT_BAD_FORMAT: return "slot format from another firmware";
    case PKGSLOT_BAD_CRC:    return "contents do not add up";
    case PKGSLOT_BAD_ABI:    return "built against an ABI this firmware cannot honour";
    case PKGSLOT_BAD_SIZE:   return "recorded extents do not fit the slot";
    case PKGSLOT_BAD_FW:     return "built against a different firmware build";
    }
    return "?";
}

static inline uint32_t round_up(uint32_t v, uint32_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

// --- routing ------------------------------------------------------------------

uint32_t pkgslot_body_capacity(uint32_t slot_bytes) {
    if (slot_bytes <= PKGSLOT_HEADER) return 0;
    // Rounded DOWN to a program page: the writer's last flush pads the tail out
    // to one, so a body that ends inside the final page still needs the whole
    // page. Reporting the unrounded figure would accept a package by a few bytes
    // and then fail programming the last page, which is the one failure this
    // whole function exists to move to before the erase.
    return (slot_bytes - PKGSLOT_HEADER) & ~(PKGSLOT_PROG - 1u);
}

PkgRoute pkg_route(const PkgRouteIn &in) {
    // Order matters only for what gets REPORTED. "too big for the slot" is worth
    // saying and the others are not: a board with no slots, or a package that
    // was never built for one, is not a problem anybody can act on — it is just
    // how that package installs. So the quiet reasons are answered first, and
    // only a package that genuinely wanted the slot and could not have it comes
    // back as TOO_BIG.
    if (!in.pic || !in.svc) return PKG_ROUTE_COPY;
    if (!in.slot_bytes || !in.slot_free) return PKG_ROUTE_COPY;
    // Rounded UP to a program page before it is compared, because that is what
    // the body will actually occupy: the writer's last flush pads its final page
    // out with 0xFF and programs the whole thing. The caller's figure is a bound
    // on the CONTENT, and a bound that ignores the padding accepts a package by
    // a hundred bytes and then fails programming the last page — after the erase,
    // which is the one place a failure costs something.
    if (round_up(in.need_bytes, PKGSLOT_PROG) > pkgslot_body_capacity(in.slot_bytes))
        return PKG_ROUTE_TOO_BIG;
    return PKG_ROUTE_SLOT;
}

const char *pkg_route_str(PkgRoute r) {
    switch (r) {
    case PKG_ROUTE_COPY:    return "copy to RAM";
    case PKG_ROUTE_SLOT:    return "flash slot";
    case PKG_ROUTE_TOO_BIG: return "too big for the slot";
    }
    return "?";
}

// --- writing ----------------------------------------------------------------

bool pkgslot_begin(PkgSlotWriter *w, const SlotFlash *fl,
                   uint32_t slot_off, uint32_t slot_bytes) {
    if (!w || !fl || !fl->erase || !fl->program) return false;
    if (slot_bytes <= PKGSLOT_HEADER) return false;
    if (slot_off % PKGSLOT_ERASE || slot_bytes % PKGSLOT_ERASE) return false;

    memset(w, 0, sizeof(*w));
    w->fl         = *fl;
    w->slot_off   = slot_off;
    w->slot_bytes = slot_bytes;

    // THE HEADER SECTOR GOES FIRST, and this ordering is the safety property.
    // Erasing it destroys the old magic before anything else is touched, so the
    // slot is "empty" for the whole of the install rather than "valid, and
    // describing a body that is being overwritten underneath it".
    if (!w->fl.erase(w->fl.ctx, slot_off, PKGSLOT_HEADER)) { w->failed = true; return false; }
    return true;
}

// Erase forward so that [0, need) of the body is erased. Lazy, because a slot is
// sized for the largest package it might ever hold and erasing 256 KB to store 8
// costs seconds of a person's time for nothing.
static bool erase_to(PkgSlotWriter *w, uint32_t need) {
    if (need <= w->erased_to) return true;
    uint32_t want = round_up(need, PKGSLOT_ERASE);
    if (PKGSLOT_HEADER + want > w->slot_bytes) return false;
    if (!w->fl.erase(w->fl.ctx, w->slot_off + PKGSLOT_HEADER + w->erased_to,
                     want - w->erased_to))
        return false;
    w->erased_to = want;
    return true;
}

// Program the page buffer, padded to a whole program unit with 0xFF — the value
// erased flash already holds, so a pad byte writes nothing and the tail of a slot
// stays erasable-looking rather than being a block of zeros.
static bool flush_page(PkgSlotWriter *w) {
    if (!w->page_fill) return true;
    uint32_t n = round_up(w->page_fill, PKGSLOT_PROG);
    memset(w->page + w->page_fill, 0xFF, n - w->page_fill);
    if (!erase_to(w, w->page_base + n)) return false;
    if (!w->fl.program(w->fl.ctx, w->slot_off + PKGSLOT_HEADER + w->page_base,
                       w->page, n))
        return false;
    // The CRC covers the bytes that were WRITTEN, pad included, so verifying it
    // needs only the body length and not a second record of where the pad began.
    w->crc = pkgslot_crc32(w->crc, w->page, n);
    w->page_base += n;
    w->page_fill = 0;
    return true;
}

// Append to the body at the cursor. Every write goes through here so there is one
// page buffer and one CRC, whoever the caller is.
static bool body_append(PkgSlotWriter *w, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        uint32_t room = PKGSLOT_PROG - w->page_fill;
        uint32_t n    = len < room ? len : room;
        memcpy(w->page + w->page_fill, p, n);
        w->page_fill += n; p += n; len -= n; w->cursor += n;
        if (w->page_fill == PKGSLOT_PROG && !flush_page(w)) return false;
    }
    return true;
}

// Pad the body forward to `to` with 0xFF, for alignment between the blob and the
// recipe arrays.
static bool body_pad_to(PkgSlotWriter *w, uint32_t to) {
    static const uint8_t ff[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    while (w->cursor < to) {
        uint32_t n = to - w->cursor;
        if (n > sizeof(ff)) n = sizeof(ff);
        if (!body_append(w, ff, n)) return false;
    }
    return true;
}

bool pkgslot_sink(void *writer, uint32_t off, const void *data, uint32_t len) {
    PkgSlotWriter *w = (PkgSlotWriter *)writer;
    if (!w || w->failed) return false;
    // Forward only, no gaps, no overlap. A producer that writes out of order
    // would silently put a page somewhere else; refusing is the only safe answer,
    // and the interface promises ascending non-overlapping writes.
    if (off != w->cursor) { w->failed = true; return false; }
    if (PKGSLOT_HEADER + w->cursor + len > w->slot_bytes) { w->failed = true; return false; }
    if (!body_append(w, data, len)) { w->failed = true; return false; }
    return true;
}

bool pkgslot_commit(PkgSlotWriter *w, const PicManifest *m) {
    if (!w || !m || w->failed) return false;

    PkgSlotMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.header    = m->header;
    meta.api_major = m->api_major;
    meta.api_minor = m->api_minor;
    // The firmware this slot's indices were resolved against, so a later boot on
    // a firmware whose ABI table has drifted refuses the slot instead of running
    // it against shifted indices. Recorded from the SAME table pkgslot_open will
    // recheck, so the two agree by construction.
    meta.abi_sym_count  = api_symbol_count();
    meta.abi_prefix_crc = api_abi_prefix_crc(meta.abi_sym_count);
    meta.got_entry_size = (uint16_t)sizeof(PicGotEntry);
    meta.abs_entry_size = (uint16_t)sizeof(PicAbs32);
    meta.ro_size   = m->ro_size;
    meta.text_size = m->text_size;
    meta.rodata_off = m->rodata_off; meta.rodata_size = m->rodata_size;
    meta.veneer_off = m->veneer_off; meta.veneer_size = m->veneer_size;
    meta.entry_off = m->entry_off;
    meta.got_bytes = m->got_bytes;
    meta.data_off  = m->data_off;
    meta.data_size = m->data_size;
    meta.ram_size  = m->ram_size;
    meta.got_count = m->got_count;
    meta.abs_count = m->abs_count;

    // The blob was written first, at body offset 0. The recipe arrays follow it,
    // each on a four-byte boundary because they are read back out of memory-mapped
    // flash as structs and an unaligned word read is a fault on some parts and
    // merely wrong on others.
    if (w->cursor != m->ro_size) { w->failed = true; return false; }
    meta.blob_off = 0;

    meta.got_off = round_up(w->cursor, 4);
    if (!body_pad_to(w, meta.got_off)) { w->failed = true; return false; }
    if (m->got_count && !body_append(w, m->got, m->got_count * (uint32_t)sizeof(PicGotEntry)))
        { w->failed = true; return false; }

    meta.abs_off = round_up(w->cursor, 4);
    if (!body_pad_to(w, meta.abs_off)) { w->failed = true; return false; }
    if (m->abs_count && !body_append(w, m->abs, m->abs_count * (uint32_t)sizeof(PicAbs32)))
        { w->failed = true; return false; }

    meta.data_init_off = round_up(w->cursor, 4);
    if (!body_pad_to(w, meta.data_init_off)) { w->failed = true; return false; }
    if (m->data_size && !body_append(w, m->data_init, m->data_size))
        { w->failed = true; return false; }

    if (!flush_page(w)) { w->failed = true; return false; }
    meta.body_bytes = w->page_base;          // what was actually programmed, pad included

    // The metadata, in the header sector but NOT in its first program page. Then
    // the commit record in that first page, on its own, last of everything.
    uint8_t metabuf[PKGSLOT_META_BYTES];
    memset(metabuf, 0xFF, sizeof(metabuf));
    memcpy(metabuf, &meta, sizeof(meta));
    if (!w->fl.program(w->fl.ctx, w->slot_off + PKGSLOT_META_OFF,
                       metabuf, PKGSLOT_META_BYTES))
        { w->failed = true; return false; }

    PkgSlotCommit c;
    memset(&c, 0, sizeof(c));
    c.magic      = PKGSLOT_MAGIC;
    c.format     = PKGSLOT_FORMAT;
    c.body_bytes = meta.body_bytes;
    c.body_crc   = w->crc;
    c.meta_crc   = pkgslot_crc32(0, metabuf, PKGSLOT_META_BYTES);
    uint8_t hdr[PKGSLOT_PROG];
    memset(hdr, 0xFF, sizeof(hdr));
    memcpy(hdr, &c, sizeof(c));
    if (!w->fl.program(w->fl.ctx, w->slot_off, hdr, PKGSLOT_PROG))
        { w->failed = true; return false; }
    return true;
}

bool pkgslot_erase(const SlotFlash *fl, uint32_t slot_off) {
    if (!fl || !fl->erase) return false;
    return fl->erase(fl->ctx, slot_off, PKGSLOT_HEADER);
}

// --- reading ----------------------------------------------------------------

PkgSlotStatus pkgslot_open(const void *base, uint32_t slot_bytes, PicManifest *m) {
    if (!base || !m || slot_bytes <= PKGSLOT_HEADER) return PKGSLOT_BAD_SIZE;
    const uint8_t *p = (const uint8_t *)base;
    PkgSlotCommit c;
    memcpy(&c, p, sizeof(c));

    // Erased flash is all ones, so this is also "nothing was ever written here"
    // and "an install began and did not finish". They are the same state on
    // purpose: both mean there is nothing to run.
    if (c.magic != PKGSLOT_MAGIC) return PKGSLOT_EMPTY;
    if (c.format != PKGSLOT_FORMAT) return PKGSLOT_BAD_FORMAT;

    if (pkgslot_crc32(0, p + PKGSLOT_META_OFF, PKGSLOT_META_BYTES) != c.meta_crc)
        return PKGSLOT_BAD_CRC;

    PkgSlotMeta meta;
    memcpy(&meta, p + PKGSLOT_META_OFF, sizeof(meta));

    // Layout the reader is about to trust. Checked before the CRC covers it is
    // used to address anything, because a corrupt body_bytes would send the CRC
    // walk off the end of the mapping and fault before it could report.
    if (c.body_bytes != meta.body_bytes) return PKGSLOT_BAD_CRC;
    if ((uint64_t)PKGSLOT_HEADER + meta.body_bytes > slot_bytes) return PKGSLOT_BAD_SIZE;
    if (meta.got_entry_size != sizeof(PicGotEntry) ||
        meta.abs_entry_size != sizeof(PicAbs32))
        return PKGSLOT_BAD_FORMAT;

    const uint8_t *body = p + PKGSLOT_HEADER;
    if (pkgslot_crc32(0, body, meta.body_bytes) != c.body_crc) return PKGSLOT_BAD_CRC;

    // Each region has to lie inside the body, and the two recipe arrays have to
    // be four-aligned — they are cast to structs and read in place.
    uint32_t got_bytes_arr = meta.got_count * (uint32_t)sizeof(PicGotEntry);
    uint32_t abs_bytes_arr = meta.abs_count * (uint32_t)sizeof(PicAbs32);
    if ((uint64_t)meta.blob_off + meta.ro_size > meta.body_bytes) return PKGSLOT_BAD_SIZE;
    if ((uint64_t)meta.got_off + got_bytes_arr > meta.body_bytes) return PKGSLOT_BAD_SIZE;
    if ((uint64_t)meta.abs_off + abs_bytes_arr > meta.body_bytes) return PKGSLOT_BAD_SIZE;
    if ((uint64_t)meta.data_init_off + meta.data_size > meta.body_bytes) return PKGSLOT_BAD_SIZE;
    if ((meta.got_off | meta.abs_off | meta.blob_off) & 3u) return PKGSLOT_BAD_SIZE;
    if (meta.blob_off != 0) return PKGSLOT_BAD_SIZE;      // what pkgslot_blob assumes
    if (meta.entry_off >= meta.ro_size) return PKGSLOT_BAD_SIZE;
    if (meta.got_count * 4u > meta.got_bytes) return PKGSLOT_BAD_SIZE;
    if ((uint64_t)meta.data_off + meta.data_size > meta.ram_size) return PKGSLOT_BAD_SIZE;

    // THE FAILURE THAT DOES NOT FAULT. A slot built against an older ABI holds
    // baked function INDICES, and an index that has moved calls the wrong
    // function perfectly happily: no fault, no message, just wrong behaviour.
    // Every other check here is belt and braces next to this one.
    if (meta.api_major != RPC_API_MAJOR || meta.api_minor > RPC_API_MINOR)
        return PKGSLOT_BAD_ABI;

    // THE SAME FAILURE ONE STEP FINER. Matching major/minor is not enough: the
    // veneers bake INDICES into this firmware's dispatch table, and a table that
    // was reordered or had a symbol removed — without a MINOR bump, which the
    // append-only rule in api.cpp is supposed to prevent but a mistake would not —
    // leaves those indices naming different functions. No fault; strcmp against a
    // package's own .rodata simply starts failing. So the running firmware
    // recomputes the identity over the SAME prefix the slot recorded and refuses
    // any slot whose indices no longer mean what they did.
    //
    // A slot written before this field existed records the erased value in both
    // words (the fixed metadata window is padded with 0xFF). Such a slot is
    // grandfathered — accepted — because the only firmware that could have written
    // it evolved the table append-only, so its indices are still valid here, and
    // its .app is on the filesystem to reinstall from if a device ever proves
    // otherwise. Every slot written by THIS firmware onward carries a real
    // identity and is checked.
    if (meta.abi_sym_count != 0xFFFFFFFFu) {
        if (meta.abi_sym_count > api_symbol_count()) return PKGSLOT_BAD_FW;
        if (api_abi_prefix_crc(meta.abi_sym_count) != meta.abi_prefix_crc)
            return PKGSLOT_BAD_FW;
    }

    memset(m, 0, sizeof(*m));
    m->header    = meta.header;
    m->api_major = meta.api_major;
    m->api_minor = meta.api_minor;
    m->ro_size   = meta.ro_size;
    m->text_size = meta.text_size;
    m->rodata_off = meta.rodata_off; m->rodata_size = meta.rodata_size;
    m->veneer_off = meta.veneer_off; m->veneer_size = meta.veneer_size;
    m->entry_off = meta.entry_off;
    m->got_bytes = meta.got_bytes;
    m->data_off  = meta.data_off;
    m->data_size = meta.data_size;
    m->ram_size  = meta.ram_size;
    m->got_count = meta.got_count;
    m->abs_count = meta.abs_count;
    // Straight into the mapping. Nothing is copied — that is the whole reason a
    // package can be resident for 60 KB instead of 180.
    m->got       = (PicGotEntry *)(void *)(uintptr_t)(body + meta.got_off);
    m->abs       = (PicAbs32 *)(void *)(uintptr_t)(body + meta.abs_off);
    m->data_init = (uint8_t *)(uintptr_t)(body + meta.data_init_off);
    m->borrowed  = true;
    return PKGSLOT_OK;
}
