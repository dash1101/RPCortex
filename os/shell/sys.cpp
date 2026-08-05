// System commands — the v1 sys_sys.py set, wording and layout preserved.
//
// These are the commands people type to find out whether the device is healthy,
// so their output is reproduced from v1 line for line: the same labels, the same
// 12-character label column, the same order. Someone who knows what `sysinfo`
// looks like on Vela should not have to re-learn it here.
//
// Where v1 reported something that has no v2 equivalent (the MicroPython build
// string) the line is replaced with the true equivalent rather than faked — a
// version field that lies is worse than one that is absent.

#include "command.h"
#include "out.h"
#include "blackbox.h"
#include "kernel.h"
#include "storage.h"
#include "session.h"
#include "registry.h"
#include "users.h"
#include "history.h"
#include "interrupt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"

// The board's image kind, for the "Image" line. One string, one place.
#if PICO_RP2040
  #define RPC_ARCH "RP2040"
#else
  #define RPC_ARCH "RP2350"
#endif

const char *fs_cwd(void);

// --- helpers ----------------------------------------------------------------

// The largest block malloc can actually hand out right now, found by probing.
//
// This is v1's _largest_block, and it matters here for the same reason: free
// bytes are not the number that predicts whether the next allocation succeeds.
// A C++ heap fragments too — newlib's malloc coalesces adjacent free chunks but
// cannot move a live one — so a device can hold 60 KB free with no single run
// big enough for a TLS record. Reporting only "free" is what made "plenty of
// memory" and "allocation failed" look like a contradiction on v1.
//
// v1's cap had to sit above any plausible answer or the probe reported its own
// ceiling as catastrophic fragmentation; the same trap applies, so the cap is
// the whole arena.
static uint32_t largest_block(void) {
    // Cap at what is actually free: a probe above that can never succeed, and
    // asking newlib for 400 KB only to be refused is wasted work. The cap must
    // still sit ABOVE any plausible answer or the probe reports its own ceiling
    // as catastrophic fragmentation, which is the trap v1 fell into.
    uint32_t lo = 0, hi = heap_free(), best = 0;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (mid == 0) break;
        void *p = malloc(mid);
        if (p) { free(p); best = mid; lo = mid + 1024; }
        else   { if (mid < 1024) break; hi = mid - 1024; }
    }
    return best;
}

static unsigned cpu_mhz(void) { return clock_get_hz(clk_sys) / 1000000u; }

// --- commands ---------------------------------------------------------------

static int cmd_uptime(int, char **) {
    uint64_t total = time_us_64() / 1000000ull;
    unsigned s = (unsigned)(total % 60);
    unsigned m = (unsigned)((total / 60) % 60);
    unsigned h = (unsigned)(total / 3600);
    if (h)      out_multi("Uptime: %uh %um %us", h, m, s);
    else if (m) out_multi("Uptime: %um %us", m, s);
    else        out_multi("Uptime: %us", s);
    return 0;
}

// date — read or set the always-on clock. The calendar API works the same on
// RP2040 (RTC) and RP2350 (powman AON timer), so this is portable. The display
// offset comes from System.TZ_Offset, the same key v1 used.
static int cmd_date(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "set")) {
        if (argc < 3) { out_warn("Usage: date set YYYY-MM-DD [HH:MM:SS]"); return 1; }
        struct tm t; memset(&t, 0, sizeof(t));
        int Y, Mo, D, H = 0, Mi = 0, S = 0;
        if (sscanf(argv[2], "%d-%d-%d", &Y, &Mo, &D) != 3) {
            out_err("Bad format. Use: date set YYYY-MM-DD HH:MM:SS"); return 1;
        }
        if (argc >= 4) sscanf(argv[3], "%d:%d:%d", &H, &Mi, &S);
        t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D;
        t.tm_hour = H; t.tm_min = Mi; t.tm_sec = S;
        t.tm_isdst = 0;
        if (!aon_timer_set_time_calendar(&t)) { out_err("Could not set the clock."); return 1; }
        // Until this happens the clock is RUNNING but not RIGHT (kboot seeds a
        // placeholder), so file timestamps are withheld. Setting it is what makes
        // them trustworthy, and therefore what turns them on.
        bool first = strcmp(reg_get("System.Clock_Set", "false"), "true") != 0;
        reg_set("System.Clock_Set", "true");
        out_ok("Clock set.");
        if (first) out_multi("  File timestamps are recorded from now on.");
        return 0;
    }
    if (argc >= 2) { out_warn("Usage: date   |   date set YYYY-MM-DD [HH:MM:SS]"); return 1; }

    struct tm t;
    if (!aon_timer_get_time_calendar(&t)) { out_err("Clock is not running."); return 1; }
    int off = (int)reg_get_int("System.TZ_Offset", 0);
    if (off) {
        // Re-normalise through the epoch so an offset can roll the date over.
        time_t e = mktime(&t) + (time_t)off * 3600;
        struct tm *l = gmtime(&e);
        if (l) t = *l;
        out_multi("%04d-%02d-%02d  %02d:%02d:%02d  (UTC%+d)",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, off);
    } else {
        out_multi("%04d-%02d-%02d  %02d:%02d:%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    }
    return 0;
}

static int cmd_sysinfo(int, char **) {
    uint32_t free_ram  = heap_free();
    uint32_t total_ram = heap_total();

    out_info("=== RPCortex %s — System Info ===", RPC_OS_CODENAME);
    out_multi("  OS Version  : %s", RPC_OS_VERSION);
    out_multi("  Codename    : %s", RPC_OS_CODENAME);
    out_multi("  Build       : %s  (%s)",
              reg_get("System.Build", "1"), reg_get("System.Stage", "dev"));
    out_multi("  Image       : C++ native (%s)", RPC_ARCH);
    const char *owner = reg_get("System.Owner", nullptr);
    if (owner && owner[0]) out_multi("  Owner       : %s", owner);
    out_multi("  Device ID   : %s", reg_get("System.Device_ID", "vela"));
    out_multi("  Active User : %s", session_user());
    out_multi("  Platform    : %s", PICO_BOARD);
    out_multi("  Compiler    : GCC %s", __VERSION__);
    out_multi("  CPU Freq    : %u MHz", cpu_mhz());
    out_multi("  Boot Clock  : %s", reg_get("Hardware.Boot_Clock", "not set"));
    out_multi("  Max Clock   : %s", reg_get("Hardware.Max_Clock", "unknown"));
    out_multi("  RAM Total   : %u KB", (unsigned)(total_ram / 1024));
    out_multi("  RAM Free    : %u KB  (%u%%)", (unsigned)(free_ram / 1024),
              (unsigned)(total_ram ? free_ram * 100 / total_ram : 0));
    out_multi("  Flash Free  : %u KB", (unsigned)(storage_free_bytes() / 1024));
    return 0;
}

uint32_t apps_pool_bytes(void);
uint32_t apps_pool_reclaim(void);

// Everything statically allocated: bss and initialised data together. The
// linker knows, so nothing here has to be kept in step by hand.
extern "C" char __bss_start__, __bss_end__, __data_start__, __end__;

static uint32_t static_ram(void) {
    uint32_t bss  = (uint32_t)(uintptr_t)(&__bss_end__ - &__bss_start__);
    uint32_t data = (uint32_t)(uintptr_t)(&__end__ - &__data_start__);
    return bss > data ? bss : data;    // __end__ covers bss on this layout
}

static int cmd_meminfo(int, char **) {
    uint32_t free  = heap_free();
    uint32_t total = heap_total();
    uint32_t alloc = total - free;
    // Used is shown in BYTES below a kilobyte. At rest almost nothing is
    // allocated, so rounding to KB printed "0 KB (0%)", which reads as broken
    // rather than as idle.
    out_multi("  Total : %u KB", (unsigned)(total / 1024));
    if (alloc < 1024)
        out_multi("  Used  : %u B  (nothing allocated right now)", (unsigned)alloc);
    else
        out_multi("  Used  : %u KB  (%u%%)", (unsigned)(alloc / 1024),
                  (unsigned)(total ? alloc * 100 / total : 0));
    out_multi("  Free  : %u KB", (unsigned)(free / 1024));
    uint32_t big = largest_block();

    // No "fragmentation %" any more, and that is the point of this pass.
    //
    // It was reported as 100 minus largest-over-free, which on a perfectly
    // healthy device reads 52% — because 91 KB of live allocations sit spread
    // through a 315 KB arena and split what is left into a few pieces. Nothing
    // is wrong with that. The percentage was not measuring a problem, it was
    // measuring the ordinary shape of a working heap, and reporting it as a
    // percentage of anything invited exactly the reading v1 got wrong.
    //
    // What matters is whether the largest piece is big enough to be useful. So
    // that is the number, and the warning fires on an absolute floor rather
    // than on a ratio that is alarming by construction.
    out_multi("  Largest block : %u KB", (unsigned)(big / 1024));
    if (big < 32 * 1024)
        out_warn("The largest single block is under 32 KB. A large allocation "
                 "will fail even though there is free memory.");

    uint32_t pool = apps_pool_bytes();
    if (pool)
        out_multi("  For packages  : %u KB  (stacks and heaps held per task)",
                  (unsigned)(pool / 1024));

    // What never reaches the heap at all.
    //
    // "The OS uses 91 KB" is a fair thing to conclude from the line above and a
    // wrong one: the heap is what is left AFTER the statically allocated
    // buffers, and those are the larger number. Showing it stops the heap
    // figure from being read as the whole story.
    out_blank();
    out_multi("  Reserved before the heap : %u KB", (unsigned)(static_ram() / 1024));
    out_multi("  %sthe network buffers, Bluetooth, the editor and the log ring; "
              "fixed at build time, not allocated%s", C_GRAY, C_RESET);
    return 0;
}

static int cmd_ver(int, char **) {
    out_multi("RPCortex %s  —  %s", RPC_OS_VERSION, RPC_OS_CODENAME);
    out_multi("Build: %s  (%s)  [C++ native]",
              reg_get("System.Build", "1"), reg_get("System.Stage", "dev"));
    out_multi("GCC %s   Platform: %s (%s)", __VERSION__, PICO_BOARD, RPC_ARCH);
    return 0;
}

static int cmd_clear(int, char **) { printf("\x1b[2J\x1b[H"); return 0; }

static int cmd_freeup(int, char **) {
    // There is no GC to run. What CAN be reclaimed is the fragmentation malloc
    // itself can coalesce, so this reports honestly rather than pretending to
    // sweep: the number that matters is the largest run, not the total.
    // The package pool first: it is the one thing the OS holds that it can give
    // back on request, and it is fifteen kilobytes a task.
    uint32_t pooled = apps_pool_reclaim();
    if (pooled)
        out_multi("  Released %u KB that was being kept for packages.",
                  (unsigned)(pooled / 1024));

    uint32_t before = largest_block();
    uint32_t free   = heap_free();
    out_ok("Heap: %u KB free, largest run %u KB.",
           (unsigned)(free / 1024), (unsigned)(before / 1024));
    if (before < 16 * 1024)
        out_warn("Largest run is under 16 KB — reboot to defragment.");
    return 0;
}

static int cmd_which(int argc, char **argv) {
    if (argc < 2) { out_warn("Usage: which <command>"); return 1; }
    const char *t = cmd_alias_target(argv[1]);
    if (t) {
        out_multi("  %s = %s  (alias)", argv[1], t);
        return 0;
    }
    const Command *c = cmd_find(argv[1]);
    if (!c) { out_warn("'%s': not found.", argv[1]); return 1; }
    out_multi("  %s : %s", c->name, c->owner ? "package command" : "built-in command");
    return 0;
}

static int cmd_history(int, char **) {
    int n = hist_count();
    if (!n) { out_multi("  (no history yet)"); return 0; }
    // hist_get(0) is the most recent; print oldest-first with 1-based numbers so
    // the listing reads like v1's.
    for (int i = n - 1; i >= 0; i--)
        out_multi("  %4d  %s", n - i, hist_get(i));
    return 0;
}

static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) { out_warn("Usage: sleep <seconds>"); return 1; }
    char *end = nullptr;
    double secs = strtod(argv[1], &end);
    if (end == argv[1] || secs < 0) { out_warn("Invalid number: '%s'", argv[1]); return 1; }
    if (secs > 3600) secs = 3600;             // a shell that sleeps for a day is a hang
    // Sliced so Ctrl+C interrupts a long sleep instead of waiting it out.
    uint32_t left = (uint32_t)(secs * 1000.0);
    while (left && !intr_check()) {
        uint32_t slice = left > 50 ? 50 : left;
        sleep_ms(slice);
        left -= slice;
    }
    return intr_pending() ? 130 : 0;
}

// env — the registry as a browsable listing, grouped by the prefix before the
// dot. v1 read the INI file and printed its sections; v2's registry is flat, so
// the prefix IS the section and grouping reproduces the same output.
static int cmd_env(int argc, char **argv) {
    char want[REG_KEY_MAX] = {0};
    if (argc >= 2) { snprintf(want, sizeof(want), "%s", argv[1]); }
    char shown[8][REG_KEY_MAX];
    uint32_t n_shown = 0;

    for (uint32_t i = 0; i < reg_count(); i++) {
        const char *k = reg_key_at(i);
        const char *dot = strchr(k, '.');
        char section[REG_KEY_MAX];
        uint32_t len = dot ? (uint32_t)(dot - k) : (uint32_t)strlen(k);
        if (len >= sizeof(section)) len = sizeof(section) - 1;
        memcpy(section, k, len); section[len] = 0;

        if (want[0] && strcasecmp(section, want) != 0) continue;

        bool seen = false;
        for (uint32_t s = 0; s < n_shown; s++)
            if (strcmp(shown[s], section) == 0) { seen = true; break; }
        if (!seen && n_shown < 8) {
            snprintf(shown[n_shown], REG_KEY_MAX, "%s", section);
            n_shown++;
            out_blank();
            out_multi("%s[%s]%s", C_CYAN, section, C_RESET);
        }
        out_multi("  %s = %s", dot ? dot + 1 : k, reg_get(k, ""));
    }
    if (!n_shown) { out_warn("No settings under '%s'.", want); return 1; }
    out_blank();
    return 0;
}

// pulse — CPU clock management, v1's command and its layout. Setting the system
// clock is a real hardware change, so a rejected frequency says so rather than
// silently leaving the old one in place.
static int cmd_pulse(int argc, char **argv) {
    if (argc < 2) {
        out_info("=== pulse — CPU clock management ===");
        out_multi("  Current    : %u MHz", cpu_mhz());
        out_multi("  Boot Clock : %s", reg_get("Hardware.Boot_Clock", "not set"));
        out_multi("  Max Clock  : %s", reg_get("Hardware.Max_Clock", "unknown"));
        out_blank();
        out_multi("  pulse status          Show clock info");
        out_multi("  pulse set <MHz>       Set clock now         (e.g. pulse set 200)");
        out_multi("  pulse boot <MHz>      Set the boot clock    (e.g. pulse boot 150)");
        return 0;
    }
    if (!strcmp(argv[1], "status")) {
        out_multi("  Current    : %u MHz", cpu_mhz());
        out_multi("  Boot Clock : %s", reg_get("Hardware.Boot_Clock", "not set"));
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc >= 3) {
        unsigned mhz = (unsigned)strtoul(argv[2], nullptr, 10);
        if (mhz < 48 || mhz > 400) { out_err("Out of range. Use 48-400 MHz."); return 1; }
        if (!set_sys_clock_khz(mhz * 1000, /*required*/false)) {
            out_err("%u MHz is not reachable from this crystal.", mhz);
            return 1;
        }
        // stdio's UART/USB divisors were computed for the old clock.
        stdio_init_all();
        out_ok("Clock set to %u MHz.", cpu_mhz());
        return 0;
    }
    if (!strcmp(argv[1], "boot") && argc >= 3) {
        unsigned mhz = (unsigned)strtoul(argv[2], nullptr, 10);
        if (mhz < 48 || mhz > 400) { out_err("Out of range. Use 48-400 MHz."); return 1; }
        reg_set("Hardware.Boot_Clock", argv[2]);
        out_ok("Boot clock set to %u MHz (applies from the next boot).", mhz);
        return 0;
    }
    out_warn("Usage: pulse [status | set <MHz> | boot <MHz>]");
    return 1;
}

// Shared, because more than one command ends in a restart and the core-1 reset
// above it is the sort of detail a second copy would quietly omit.
void sys_reboot(void) {
    bb_note_clean_exit();        // on purpose, so the next boot does not report it
    kboot_expect_reboot();       // ...nor announce the watchdog that carries it out
    sleep_ms(120);
    multicore_reset_core1();     // as above: do not reset around a live core 1
    watchdog_reboot(0, 0, 0);
    while (1) {}
}

static int cmd_reboot(int, char **) {
    out_info("Rebooting system...");
    sys_reboot();
    while (1) {}
}

// bootloader — reboot into the ROM's USB mass-storage mode, the same state
// BOOTSEL-at-power-on produces. This is the v2 replacement for v1's `rawrepl`:
// on MicroPython the way to reflash was to drop out of the OS to the REPL so a
// host tool could talk to it, and here the equivalent is handing the USB port
// back to the bootrom so a .uf2 can be dragged on.
//
// The device disappears from the serial port the moment this runs, so it says
// what is about to happen first — a console that goes dead with no explanation
// reads as a crash.
static int cmd_bootloader(int, char **) {
    out_info("Rebooting into the bootloader...");
    out_multi("  The serial port will drop and an RPI-RP2 drive will appear.");
    out_multi("  Copy a .uf2 onto it, or power-cycle to come back.");
    sleep_ms(400);          // let the message reach the terminal before USB goes

    // Stop core 1 first. The bootrom takes over the whole chip, and leaving a
    // second core executing from flash while it does means the reset does not
    // complete — the device sat there frozen instead of coming back as a drive.
    // The same applies to a plain reboot.
    multicore_reset_core1();
    rom_reset_usb_boot(0, 0);
    while (1) {}
}

// A "soft" reboot on bare metal is still a reset — there is no interpreter to
// drop back into. It is kept as a separate command because muscle memory from v1
// reaches for it, and doing the sensible thing beats "unknown command".
static int cmd_sreboot(int, char **) {
    out_info("Performing soft reboot...");
    sleep_ms(120);
    multicore_reset_core1();
    watchdog_reboot(0, 0, 0);
    while (1) {}
}

void sys_register(void) {
    static const Command cmds[] = {
        {"uptime",  "time since boot",              cmd_uptime,  nullptr},
        {"date",    "date [set YYYY-MM-DD ..]",     cmd_date,    nullptr, LEVEL_ADMIN},
        {"sysinfo", "system overview",              cmd_sysinfo, nullptr},
        {"meminfo", "RAM usage and fragmentation",  cmd_meminfo, nullptr},
        {"ver",     "version and board",            cmd_ver,     nullptr},
        {"clear",   "clear the screen",             cmd_clear,   nullptr},
        {"freeup",  "report reclaimable memory",    cmd_freeup,  nullptr},
        {"which",   "which <command>",              cmd_which,   nullptr},
        {"history", "recent commands",              cmd_history, nullptr},
        {"sleep",   "sleep <seconds>",              cmd_sleep,   nullptr},
        {"env",     "registry settings by section", cmd_env,     nullptr, LEVEL_ADMIN},
        {"pulse",   "CPU clock management",         cmd_pulse,   nullptr, LEVEL_ADMIN},
        {"reboot",  "restart the device",           cmd_reboot,  nullptr, LEVEL_ADMIN},
        {"sreboot", "restart the device",           cmd_sreboot, nullptr, LEVEL_ADMIN},
        {"bootloader", "reboot into USB flashing mode", cmd_bootloader, nullptr, LEVEL_ADMIN},
    };
    for (const auto &c : cmds) cmd_register(&c);

    cmd_alias("cls",       "clear");
    cmd_alias("version",   "ver");
    cmd_alias("uname",     "ver");
    cmd_alias("free",      "meminfo");
    cmd_alias("gc",        "freeup");
    cmd_alias("softreset", "sreboot");
    // v1's route out of the OS for reflashing was `rawrepl`; here it is the
    // bootrom. Same intent, so the same name reaches it.
    cmd_alias("rawrepl",  "bootloader");
    cmd_alias("bootsel",  "bootloader");
    cmd_alias("flash",    "bootloader");
}
