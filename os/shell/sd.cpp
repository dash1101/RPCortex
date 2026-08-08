// `sd` — the memory card.
//
// Four subcommands, and between them they answer the four questions somebody
// with a card actually has: is it there, why is it not, what is on it, and can
// I have the bus back.
//
// The card is browsed with `ls /sd`, read with `cat /sd/...` and copied off with
// `cp /sd/... /...` — the ordinary commands, because "/sd" is routed through the
// same storage_* calls flash goes through. This command exists for the things
// those cannot express.
//
// Compiled only where RPC_HAS_SD is set; see os/CMakeLists.txt. On a board
// without it the command is not registered at all rather than existing to say
// no, which is the same choice `bt` makes the other way and the opposite of
// what a Pico W with 66 KB of headroom can afford.
#include "command.h"
#include "out.h"
#include "sdcard.h"

#include <stdio.h>
#include <string.h>

#if defined(RPC_HAS_SD) && RPC_HAS_SD

// Bytes as something a person reads. Cards are sold in powers of ten and
// formatted in powers of two, so a 16 GB card shows about 14.8 GiB of volume —
// that is not a bug and the units are printed so it does not look like one.
static void human(uint64_t bytes, char *out, unsigned cap) {
    if (bytes >= (1ull << 30))
        snprintf(out, cap, "%u.%01u GiB", (unsigned)(bytes >> 30),
                 (unsigned)(((bytes >> 20) % 1024) * 10 / 1024));
    else if (bytes >= (1ull << 20))
        snprintf(out, cap, "%u.%01u MiB", (unsigned)(bytes >> 20),
                 (unsigned)(((bytes >> 10) % 1024) * 10 / 1024));
    else if (bytes >= 1024)
        snprintf(out, cap, "%u KiB", (unsigned)(bytes >> 10));
    else
        snprintf(out, cap, "%u B", (unsigned)bytes);
}

static void say_absent(const SdInfo &i) {
    if (i.manual)                out_multi("  Card      unmounted (by hand; `sd mount` to bring it back)");
    else if (i.recently_removed) out_multi("  Card      removed");
    else                         out_multi("  Card      none  (%s)", i.last_error);
}

static int cmd_status(void) {
    SdInfo i;
    sd_info(&i);
    out_info("microSD");
    if (!i.mounted) { say_absent(i); return i.recently_removed || i.manual ? 0 : 1; }

    char cap[24], vol[24], freeb[24];
    human(i.card_bytes, cap, sizeof(cap));
    human(i.volume_bytes, vol, sizeof(vol));
    out_multi("  Card      %s, %s", i.card_type, cap);
    out_multi("  Volume    %s, %s%s%s", i.fs_type, vol,
              i.label[0] ? ", labelled " : "", i.label[0] ? i.label : "");
    if (i.free_bytes) {
        human(i.free_bytes, freeb, sizeof(freeb));
        out_multi("  Free      %s", freeb);
    } else {
        out_multi("  Free      not known (the volume keeps no count)");
    }
    out_multi("  Mounted   at %s", SD_ROOT);
    return 0;
}

static int cmd_info(void) {
    SdInfo i;
    sd_info(&i);
    out_info("microSD");
    out_multi("  SPI       bus %d, sck %d, mosi %d, miso %d, cs %d",
              i.spi_bus, i.pin_sck, i.pin_mosi, i.pin_miso, i.pin_cs);
    if (i.pin_cd >= 0) out_multi("  Detect    gpio %d", i.pin_cd);
    else               out_multi("  Detect    no card-detect pin; found by probing");

    if (!i.mounted) { say_absent(i); return 1; }

    char cap[24], vol[24];
    human(i.card_bytes, cap, sizeof(cap));
    human(i.volume_bytes, vol, sizeof(vol));
    out_multi("  Card      %s, %s", i.card_type, cap);
    out_multi("  Volume    %s, %s, starts at LBA %u",
              i.fs_type, vol, (unsigned)i.part_lba);
    out_multi("  Label     %s", i.label[0] ? i.label : "(none)");
    out_multi("  Blocks    %u read", (unsigned)i.blocks_read);
    out_multi("  Errors    %u bad CRC, %u retried",
              (unsigned)i.crc_errors, (unsigned)i.retries);
    if (i.crc_errors)
        out_warn("Bad CRCs mean the bus, not the card. Try a slower clock.");
    out_multi("  Access    read-only");
    return 0;
}

static int cmd_sd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (!strcmp(sub, "status")) return cmd_status();
    if (!strcmp(sub, "info"))   return cmd_info();

    if (!strcmp(sub, "mount")) {
        if (sd_mount()) { SdInfo i; sd_info(&i);
            out_ok("Card mounted at %s (%s, %s)", SD_ROOT, i.card_type, i.fs_type);
            return 0; }
        SdInfo i; sd_info(&i);
        out_err("No card mounted: %s", i.last_error);
        return 1;
    }

    if (!strcmp(sub, "unmount") || !strcmp(sub, "umount")) {
        if (!sd_present()) { out_warn("No card is mounted."); return 0; }
        sd_unmount(true);
        out_ok("Card unmounted. It is safe to remove.");
        return 0;
    }

    out_multi("Usage:");
    out_multi("  sd status              is there a card, and what is on it");
    out_multi("  sd info                pins, capacity, and how the bus is behaving");
    out_multi("  sd mount               look for a card now");
    out_multi("  sd unmount             let go of it before pulling it out");
    out_multi("  Browse it with `ls %s`, read with `cat`, copy off with `cp`.",
              SD_ROOT);
    return argc > 1 ? 1 : 0;
}

void sd_register(void) {
    static const Command c{"sd", "microSD card status and mounting", cmd_sd,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}

#endif  // RPC_HAS_SD
