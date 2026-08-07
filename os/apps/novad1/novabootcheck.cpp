// Desc: The startup check — what the device found, while it was finding it.
// File: novabootcheck.cpp
#include "novabootcheck.h"
#include "novagui.h"
#include "novacore.h"
#include "novaboard.h"
#include "novamodtab.h"
#include "novapower.h"
#include "novalog.h"
#include "display.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {

// One row per check. The order is deliberate: the things everything else depends
// on first, so a failure appears before the checks it would have invalidated.
static const char *const kNames[BootCheckScreen::STEPS] = {
    "Display",
    "Storage",
    "Memory",
    "Controls",
    "Clock",
    "Power",
    "Network",
    "Modules",
    "Ready",
};

#define R_BAD  0
#define R_GOOD 1
#define R_SKIP 2

void BootCheckScreen::enter(void) {
    step_ = 0;
    done_ = false;
    hold_ = 0;
    spin_ = 0;
    intro_ = 0;
    for (int i = 0; i < STEPS; i++) { result_[i] = -1; detail_[i][0] = 0; }
}

void BootCheckScreen::run_step(int i) {
    char *d = detail_[i];
    const unsigned cap = sizeof(detail_[0]);

    switch (i) {
        case 0: {   // Display — it is already up, or nothing would be visible
            Display &dp = display();
            result_[i] = dp.ready() ? R_GOOD : R_BAD;
            if (dp.ready()) snprintf(d, cap, "%s 0x%02x", dp.kind_name(), dp.address());
            else            nova::copy(d, cap, "no panel");
            break;
        }
        case 1: {   // Storage — the tree exists and there is room in it
            nova::paths_init();
            bool ok = fw_file_exists(NOVA_ROOT) != 0;
            result_[i] = ok ? R_GOOD : R_BAD;
            nova::copy(d, cap, ok ? NOVA_ROOT : "no /nova");
            break;
        }
        case 2: {   // Memory — free, and the largest single block
            uint32_t free_kb = fw_heap_free() / 1024u;
            uint32_t big_kb  = fw_heap_largest() / 1024u;
            snprintf(d, cap, "%u KB, max %u", (unsigned)free_kb, (unsigned)big_kb);
            // Under 16 KB is not enough to open a TLS connection or load a
            // second package, which is a real limit rather than a round number.
            result_[i] = free_kb >= 16 ? R_GOOD : R_BAD;
            break;
        }
        case 3: {   // Controls — the encoder and buttons have pins
            int a = board::pin(board::PIN_ENC_A);
            int sw = board::pin(board::PIN_ENC_SW);
            if (a == board::PIN_NONE || sw == board::PIN_NONE) {
                result_[i] = R_BAD;
                nova::copy(d, cap, "not wired");
            } else {
                // Whether the encoder TURNS cannot be known without somebody
                // turning it, so this reports what it is rather than claiming
                // more. The Hardware screen is where a live test belongs.
                result_[i] = R_GOOD;
                snprintf(d, cap, "EC11 on %d/%d", a, board::pin(board::PIN_ENC_B));
            }
            break;
        }
        case 4: {   // Clock — set, or running from nothing
            FwTime t;
            if (fw_time_get(&t)) {
                result_[i] = R_GOOD;
                snprintf(d, cap, "%02d:%02d %d-%02d-%02d", t.hour, t.minute,
                         t.year, t.month, t.day);
            } else {
                // Not a failure. A device with no RTC and no network has no way
                // to know the time and works perfectly well without it.
                result_[i] = R_SKIP;
                nova::copy(d, cap, "not set");
            }
            break;
        }
        case 5: {   // Power — USB, a battery, or no way to tell
            switch (power::source()) {
                case power::PWR_USB:
                    result_[i] = R_GOOD;
                    nova::copy(d, cap, "USB");
                    break;
                case power::PWR_BATTERY: {
                    int p = power::percent();
                    result_[i] = power::low() ? R_BAD : R_GOOD;
                    snprintf(d, cap, "battery %d%%", p);
                    break;
                }
                default:
                    result_[i] = R_SKIP;
                    nova::copy(d, cap, "no sense pin");
                    break;
            }
            break;
        }
        case 6: {   // Network — joined, or not
            if (fw_net_connected()) {
                char ssid[34];
                fw_net_ssid(ssid, sizeof(ssid));
                result_[i] = R_GOOD;
                nova::ellipsize(d, cap, ssid, cap - 1);
            } else {
                // Also not a failure. Plenty of what this device does needs no
                // network at all, and marking it red at every boot would teach
                // people to ignore the colour.
                result_[i] = R_SKIP;
                nova::copy(d, cap, "not joined");
            }
            break;
        }
        case 7: {   // Modules — how many answered
            modules_scan();
            int found = 0, wired = 0;
            const Module *m = modules();
            for (unsigned k = 0; k < module_count(); k++) {
                if (!module_wired(m[k])) continue;
                wired++;
                if (module_presence(m[k]) == MOD_PRESENT) found++;
            }
            snprintf(d, cap, "%d of %d answered", found, wired);
            result_[i] = R_GOOD;
            break;
        }
        default: {  // Ready — and what to say if it is not
            int bad = 0;
            for (int k = 0; k < STEPS - 1; k++) if (result_[k] == R_BAD) bad++;
            result_[i] = bad ? R_BAD : R_GOOD;
            if (bad) snprintf(d, cap, "%d problem%s", bad, bad == 1 ? "" : "s");
            else     nova::copy(d, cap, "");
            char line[48];
            snprintf(line, sizeof(line), "boot check: %d problem(s)", bad);
            log::write(line);
            break;
        }
    }
}

bool BootCheckScreen::tick(uint32_t dt_ms) {
    spin_ += dt_ms;

    // The wordmark plays FIRST, on this screen, and the checks begin underneath
    // it rather than after it. One boot screen rather than two: the second one
    // appearing was the jarring part, and running the checks behind the name is
    // what the MicroPython suite meant by the splash playing over the real work.
    if (intro_ < INTRO_MS) {
        intro_ += dt_ms;
        // The first two checks are the ones nothing can proceed without, so
        // they run behind the animation rather than waiting for it.
        if (intro_ >= INTRO_MS / 2 && step_ < 2) { run_step(step_); step_++; }
        return true;
    }

    if (done_) {
        hold_ += dt_ms;
        // Long enough to read the last few rows, short enough that somebody who
        // has seen it a hundred times is not waiting on it. A gesture skips it.
        if (hold_ >= 900) { gui::pop(); return true; }
        return true;
    }

    // ONE STEP PER FRAME. The bar advances because a check finished, not because
    // time passed — so a slow step shows as a pause on the row it is actually
    // on, which is information rather than a stall.
    if (step_ < STEPS) {
        run_step(step_);
        step_++;
        if (step_ >= STEPS) done_ = true;
    }
    return true;
}

ui::Action BootCheckScreen::on_event(Event e) {
    (void)e;
    // Any gesture skips the rest. The checks that have not run yet are the ones
    // nothing depends on being told about — everything they set up is done
    // lazily by whatever needs it.
    gui::pop();
    return ui::ACT_STAY;
}

void BootCheckScreen::draw(Canvas &c) {
    if (intro_ < INTRO_MS) { draw_intro(c); return; }

    // The last five rows, so the newest is always visible and the list appears
    // to scroll up under the bar as it fills.
    const int rows = 5;
    int first = step_ > rows ? step_ - rows : 0;

    int y = 2;
    for (int i = first; i < step_ && i < STEPS; i++) {
        const char *mark = result_[i] == R_GOOD ? "+"
                         : result_[i] == R_BAD  ? "!" : "-";
        c.text(0, y, mark, 1);
        c.text(8, y, kNames[i], 1);
        int w = c.text_width(detail_[i], 1, true);
        c.text(c.width() - w, y, detail_[i], 1, 1, true);
        y += ui::ROWH;
    }

    // The bar. Its width IS the progress — no percentage next to it, because the
    // number and the bar would be saying the same thing and one of them is
    // faster to read.
    const int by = c.height() - 8;
    c.rounded_rect(0, by, c.width(), 7, 1, false);
    int fill = step_ * (c.width() - 4) / STEPS;
    if (fill > 0) c.fill_rect(2, by + 2, fill, 3, 1);

    if (done_) {
        bool bad = result_[STEPS - 1] == R_BAD;
        c.fill_rect(0, by - ui::ROWH - 1, c.width(), ui::ROWH, 0);
        c.text_centred(by - ui::ROWH, bad ? detail_[STEPS - 1] : "Ready", 1);
    }
}

// The opening beat. Slower than it was: the first version ran the whole thing
// in 1.8 seconds and read as a flicker rather than as a reveal. A wipe wants
// long enough for the eye to follow the edge, which is most of a second on its
// own.
void BootCheckScreen::draw_intro(Canvas &c) {
    const int cx = c.width() / 2;
    const int cy = c.height() / 2;

    // A ring opening out. On a 1-bit panel there is no fade, so it thins by
    // simply not redrawing the earlier circles — which reads as a pulse rather
    // than as a growing target.
    if (intro_ < RING_MS) {
        int r = (int)(intro_ * 26 / RING_MS);
        c.circle(cx, cy, r, 1);
        if (r > 7) c.circle(cx, cy, r - 7, 1);
        return;
    }

    // The name, wiped in from the left.
    uint32_t into = intro_ - RING_MS;
    int full  = c.text_width("Nova D1", 2, false);
    int x0    = cx - full / 2;
    uint32_t span = NAME_MS - RING_MS;
    int shown = span ? (int)(into * (uint32_t)full / span) : full;
    if (shown > full) shown = full;
    c.text(x0, cy - 13, "Nova D1", 1, 2, false);
    if (shown < full) c.fill_rect(x0 + shown, cy - 15, full - shown + 2, 19, 0);

    if (intro_ >= NAME_MS) {
        c.text_centred(cy + 7, "RPCortex", 1);
        uint32_t sig = intro_ - NAME_MS;
        uint32_t sspan = INTRO_MS - NAME_MS;
        int w = sspan ? (int)(sig * 44u / sspan) : 44;
        if (w > 44) w = 44;
        c.hline(cx - w / 2, cy + 7 + FONT_H + 2, w, 1);
    }
}

}  // namespace nova
