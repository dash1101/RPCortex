#ifndef RPC_PKGSLOT_H
#define RPC_PKGSLOT_H

#include <stdint.h>
#include "loader.h"

// A package that runs from flash: the on-flash format, and the write protocol
// that survives losing power halfway through.
//
// A position-independent package splits in two. The read-only half — .text,
// .rodata and the SVC veneers — holds no absolute address, so it can be written
// to flash once and executed in place. Only the writable half is resident, which
// for Nova D1 is 60 KB instead of 180. app_pic_install produces that blob and a
// recipe for the writable half; this is where both are put so that a later boot
// can find them without the .app file, and without trusting anything.
//
// WHY THIS FILE IS SEPARATE FROM storage.cpp
//
// The format and the commit ordering are the parts that can be got wrong in ways
// no board would show you — a slot that validates when it should not, a torn
// write that looks complete. They are pure logic over a byte array, so they live
// here, take their flash operations as function pointers, and are tested on the
// host against a fake flash that models erase-to-0xFF and program-once. What is
// left in storage.cpp is three functions that touch the chip.
//
// THE COMMIT ORDER, which is the whole of the safety argument
//
//   1. Erase the slot's FIRST sector. That destroys the old magic before a
//      single byte of the new package is written, so from this moment on the
//      slot reads as empty rather than as whatever it used to hold. Getting
//      this backwards is the failure that matters: a slot still carrying a
//      valid magic over a half-written body would be loaded and run.
//   2. Erase and program the body — blob, then the GOT recipe, then the ABS32
//      recipe, then the .data initialisers. Sectors are erased as the cursor
//      first enters them, so a small package does not pay to erase a big slot.
//   3. Program the metadata, everything except the commit record.
//   4. Program the commit record — magic and CRCs — LAST, in its own 256-byte
//      program page.
//
// Erased flash is 0xFF, so a valid magic can only be there if step 4 ran, and
// step 4 only runs if the rest did. Interrupted anywhere, the slot is empty and
// the .app file it was made from is still on the filesystem: the recovery is to
// install again. There is nothing to roll back.
//
// Interrupted DURING step 1 is the one case the fake flash cannot model, and it
// is safe for a different reason: erase only turns bits on, and nothing else has
// been written yet, so the slot either still describes the package it held (which
// is intact, because the body is untouched) or has lost enough of its header to
// fail the magic or the metadata CRC. Both are answers; neither runs a hybrid.
//
// DEVICE-UNCONFIRMED: erase and program themselves, and executing from the slot.
// Everything in this file is exercised on the host against a fake flash; that
// proves the format and the ordering, not the chip.

// The erase unit and the program unit of RP2 flash. A slot begins on an erase
// boundary and its header occupies a whole erase sector, so committing the
// header can never disturb the body.
#define PKGSLOT_ERASE   4096u
#define PKGSLOT_PROG    256u
#define PKGSLOT_HEADER  PKGSLOT_ERASE      // header sector; body starts after it

#define PKGSLOT_MAGIC   0x534b5052u        // 'RPKS'
#define PKGSLOT_FORMAT  1u

// The metadata lives at a FIXED offset for a FIXED length, rather than at
// sizeof(PkgSlotMeta). The CRC covers that fixed range, so adding a field to the
// struct cannot quietly change what the CRC was taken over; it changes the
// struct, which is what PKGSLOT_FORMAT is for.
#define PKGSLOT_META_OFF    PKGSLOT_PROG   // page 0 is the commit record
#define PKGSLOT_META_BYTES  512u

// How a slot region is written. Offsets are relative to the base of the package
// slot REGION, not to the flash chip — the region's own placement is storage.cpp's
// business and nothing here needs to know it.
struct SlotFlash {
    void *ctx;
    // Erase [off, off+len). Both are multiples of PKGSLOT_ERASE. After it, every
    // byte in the range reads 0xFF.
    bool (*erase)(void *ctx, uint32_t off, uint32_t len);
    // Program len bytes at off. Both are multiples of PKGSLOT_PROG, and the range
    // has been erased and not programmed since.
    bool (*program)(void *ctx, uint32_t off, const void *data, uint32_t len);
};

// Why a slot did not open. Told apart because they mean different things to the
// person holding the board: EMPTY is a slot nobody has used, TORN is an install
// that was interrupted, and BAD_ABI is a slot from an older firmware — the one
// that would otherwise not fault, because a stale ABI index calls the wrong
// function rather than crashing.
enum PkgSlotStatus {
    PKGSLOT_OK = 0,
    PKGSLOT_EMPTY,          // no magic: never written, or erased at the start of one
    PKGSLOT_BAD_FORMAT,     // a slot from a firmware whose slot layout differs
    PKGSLOT_BAD_CRC,        // magic present, contents do not add up
    PKGSLOT_BAD_ABI,        // built against an ABI this firmware cannot honour
    PKGSLOT_BAD_SIZE,       // the recorded extents do not fit the slot
};

// The commit record. Its own program page, and the last thing written.
struct PkgSlotCommit {
    uint32_t magic;          // PKGSLOT_MAGIC — the only proof the install finished
    uint32_t format;         // PKGSLOT_FORMAT
    uint32_t body_bytes;     // bytes of body covered by body_crc
    uint32_t body_crc;
    uint32_t meta_crc;       // over PkgSlotMeta as programmed
    uint32_t reserved[3];
};

// Everything app_pic_load needs, in the slot itself. A slot is self-describing:
// it cannot disagree with the filesystem, because the filesystem is not consulted.
struct PkgSlotMeta {
    RpcAppHeader header;               // name, version, ABI — what the package IS
    uint16_t api_major, api_minor;     // what it was BUILT against
    // The two recipe arrays are read straight out of memory-mapped flash as
    // PicGotEntry[] and PicAbs32[], so their in-memory layout is part of the
    // format. Recorded and checked, because a struct that gains a field without
    // an ABI bump would otherwise be read as the old one and resolve garbage.
    uint16_t got_entry_size, abs_entry_size;
    uint32_t ro_size, text_size;
    uint32_t rodata_off, rodata_size;
    uint32_t veneer_off, veneer_size;
    uint32_t entry_off;
    uint32_t got_bytes, data_off, data_size, ram_size;
    uint32_t got_count, abs_count;
    // Where each piece sits, measured from the start of the BODY.
    uint32_t blob_off, got_off, abs_off, data_init_off;
    uint32_t body_bytes;
};

// Streaming writer. One slot, forward only.
struct PkgSlotWriter {
    SlotFlash fl;
    uint32_t  slot_off;        // region-relative base of the slot
    uint32_t  slot_bytes;
    uint32_t  cursor;          // next body byte expected, body-relative
    uint32_t  erased_to;       // body bytes whose sector is already erased
    uint32_t  crc;             // running CRC over the body as programmed
    uint32_t  page_base;       // body offset of the page being filled
    uint32_t  page_fill;
    bool      failed;
    uint8_t   page[PKGSLOT_PROG];
};

// Erase the header sector — killing the old magic first — and arm the writer.
bool pkgslot_begin(PkgSlotWriter *w, const SlotFlash *fl,
                   uint32_t slot_off, uint32_t slot_bytes);

// A SlotWrite sink over a PkgSlotWriter: pass this and the writer to
// app_pic_install. Offsets are body-relative, ascending and non-overlapping;
// anything else is refused rather than written to the wrong place.
bool pkgslot_sink(void *writer, uint32_t off, const void *data, uint32_t len);

// Append the recipe arrays after the blob, then program the metadata and, last,
// the commit record. After this returns true the slot is loadable.
bool pkgslot_commit(PkgSlotWriter *w, const PicManifest *m);

// Read a slot that is memory-mapped at `base` — XIP flash on the device, a plain
// buffer in the host test. On PKGSLOT_OK, `m` is filled with its array pointers
// aimed INTO the mapping: nothing is copied, and m->borrowed is set so
// app_pic_manifest_free will not try to release flash.
PkgSlotStatus pkgslot_open(const void *base, uint32_t slot_bytes, PicManifest *m);

// The blob inside a mapped slot — what app_pic_load's `slot` argument wants.
// Only meaningful once pkgslot_open has returned PKGSLOT_OK for that slot, which
// is also where blob_off is checked to be the zero this assumes.
static inline const void *pkgslot_blob(const void *base) {
    return (const uint8_t *)base + PKGSLOT_HEADER;
}

// Erase a slot's header sector, so it reads empty. Used to retire a package
// without rewriting the whole slot.
bool pkgslot_erase(const SlotFlash *fl, uint32_t slot_off);

const char *pkgslot_status_str(PkgSlotStatus s);

// --- which way an install goes ------------------------------------------------
//
// Decided BEFORE anything is erased, and kept here — pure, with no filesystem
// and no chip behind it — because it is the part worth testing. Everything the
// decision needs is a handful of numbers the caller has already got: whether the
// package is position-independent, whether this board reaches firmware by index
// or by address, and whether there is a slot it may have.
//
// The default is COPY. A board with no slots, a package that was not built
// -fPIC, a firmware running DIRECT veneers and a slot already spoken for all
// arrive at the same place: the copy-to-RAM path, behaving exactly as it did
// before slots existed. Only the case that is provably better takes the new one.
enum PkgRoute {
    PKG_ROUTE_COPY = 0,     // app_load into RAM, as always
    PKG_ROUTE_SLOT,         // write the flash slot and run from it
    PKG_ROUTE_TOO_BIG,      // it would take the slot, but it will not fit one
};

struct PkgRouteIn {
    bool     pic;           // reaches its globals through a GOT, and every
                            // relocation is one the slot producer can emit
    bool     svc;           // firmware is called by ABI index, not by address.
                            // A DIRECT veneer holds an address that changes with
                            // every firmware build, and baking one into flash
                            // would call the wrong function after an update.
    bool     slot_free;     // the chosen slot is empty, unreadable, or already
                            // holds THIS package (an upgrade in place)
    uint32_t slot_bytes;    // 0 when the board has no slots at all
    uint32_t need_bytes;    // app_pic_measure's body_bound
};

// The most body a slot of this size can hold, after the header sector and the
// round up to a whole program page that the writer's last flush performs.
uint32_t pkgslot_body_capacity(uint32_t slot_bytes);

PkgRoute pkg_route(const PkgRouteIn &in);
const char *pkg_route_str(PkgRoute r);

// CRC-32 (the reflected 0xEDB88320 polynomial, as zip and PNG use). Exposed
// because the test computes expected values with it.
uint32_t pkgslot_crc32(uint32_t crc, const void *data, uint32_t len);

#endif  // RPC_PKGSLOT_H
