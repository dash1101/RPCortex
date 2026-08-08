// The microSD card, as the rest of the firmware sees it.
//
// Three layers, and this is the top one:
//
//   core/sdproto.*  the SD command protocol over SPI. No hardware, host-tested
//                   against a fake card (sdproto_test).
//   core/fatro.*    a read-only FAT12/16/32 reader over a sector callback. No
//                   hardware, host-tested against volumes fsck.fat approves of
//                   (fatro_test).
//   sdcard_rp2.cpp  this: the RP2 SPI transport, the mount, and the routing
//                   that makes "/sd/..." reach the card instead of littlefs.
//
// COMPILED ONLY WHERE IT FITS. RPC_HAS_SD is set by os/CMakeLists.txt for the
// RP2350 boards — pico2_w and pico2, which is what Nova D1 targets. A Pico W
// has 66 KB of firmware headroom and no room for a second filesystem it has no
// slot to put a card in. Everything below is inside that guard, and the two
// call sites outside this file (storage.cpp's routing and shell.cpp's
// registration) are guarded the same way.
#ifndef RPC_SDCARD_H
#define RPC_SDCARD_H

#include <stdint.h>

// Same shape as StorageWalkFn, declared rather than included so this header
// does not drag storage.h (and loader.h behind it) into everything.
typedef void (*SdWalkFn)(void *ctx, const char *name, bool is_dir, uint32_t size);

// The prefix a card lives under. One definition; the shell, the ABI and the
// routing all take it from here.
#define SD_ROOT "/sd"

// Does this path belong to the card? Exact: "/sd", "/sd/" and "/sd/anything"
// do; "/sdcard" and "/sdx" are ordinary flash paths and must stay that way.
bool sd_owns_path(const char *path);

// Try to bring a card up now. Safe to call when one is already mounted (it says
// true and does nothing). Clears the "unmounted on purpose" flag.
bool sd_mount(void);

// Let go of the card. `manual` marks it as a deliberate unmount, which stops
// autodetect from mounting it straight back and takes the row out of the
// browser immediately rather than showing it as removed.
void sd_unmount(bool manual);

// Is there a mounted, readable card right now?
bool sd_present(void);

// Notice an insertion or a removal.
//
// Rate limited, and called from the paths that would care anyway — a routed
// filesystem operation and fw_storage_roots. There is deliberately NO polling
// task: a task costs a stack and wakes a sleeping device to ask a question
// nobody is waiting for the answer to, and the moment a card's state matters is
// the moment something looks at it.
void sd_poll(void);

// --- routed operations -------------------------------------------------------
//
// These mirror the storage_* calls of the same name and are what storage.cpp
// hands a "/sd" path to. Paths are absolute and include the "/sd" prefix.
bool     sd_walk(const char *path, SdWalkFn cb, void *ctx);
bool     sd_stat(const char *path, bool *is_dir, uint32_t *size);
uint32_t sd_read_at(const char *path, uint32_t off, uint8_t *buf, uint32_t cap);
uint32_t sd_mtime(const char *path);

// Copy a file OFF the card into a flash path. The mount is read-only, so this
// is the one direction that works — and it is most of why a card is wanted.
bool sd_copy_out(const char *from, const char *to);

// --- what to report ----------------------------------------------------------

struct SdInfo {
    bool     mounted;
    bool     recently_removed;   // gone, but recently enough to still say so
    bool     manual;             // unmounted on purpose
    const char *card_type;       // "SDHC/SDXC", "SDSC (v2)", ...
    const char *fs_type;         // "FAT32", ...
    const char *last_error;      // why the last mount attempt failed
    char     label[12];
    uint64_t card_bytes;         // the card
    uint64_t volume_bytes;       // the filesystem on it
    uint64_t free_bytes;         // 0 when not known
    uint32_t crc_errors;
    uint32_t retries;
    uint32_t blocks_read;
    uint32_t part_lba;
    // Where the pins came from, for when it does not work.
    int      pin_cs, pin_sck, pin_mosi, pin_miso, pin_cd;
    int      spi_bus;
};

void sd_info(SdInfo *out);

#endif  // RPC_SDCARD_H
