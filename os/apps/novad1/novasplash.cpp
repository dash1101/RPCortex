// Desc: The boot animation.
// File: novasplash.cpp
#include "novasplash.h"
#include "novagui.h"

namespace nova {

// The whole sequence, in milliseconds. Three beats: a ring opens out, the name
// wipes in behind it, and the OS underneath signs it.
#define T_RING   500
#define T_NAME   900
#define T_SIGN  1300
#define T_END   1800

void SplashScreen::enter(void) { t_ = 0; }

bool SplashScreen::tick(uint32_t dt_ms) {
    t_ += dt_ms;
    if (t_ >= T_END) { gui::pop(); return true; }
    return true;      // every frame, for the whole of it — this is the one
                      // screen where continuous animation is the point
}

ui::Action SplashScreen::on_event(Event e) {
    // ANY gesture skips it. Somebody who has seen it four hundred times and is
    // reaching for the device should not have to watch it again, and a splash
    // that cannot be skipped is the most irritating kind.
    (void)e;
    gui::pop();
    return ui::ACT_STAY;
}

void SplashScreen::draw(Canvas &c) {
    const int cx = c.width() / 2;
    const int cy = c.height() / 2;

    // A ring that opens out and fades by thinning: on a 1-bit panel there is no
    // fade, so the ring gets larger and the earlier circles are simply not
    // redrawn. Reads as a pulse rather than as a growing target.
    if (t_ < T_NAME) {
        int r = (int)(t_ * 26 / T_RING);
        if (r > 26) r = 26;
        c.circle(cx, cy, r, 1);
        if (r > 6) c.circle(cx, cy, r - 6, 1);
    }

    // The name wipes in from the left rather than appearing: a reveal is what
    // makes 400 ms feel like something happening instead of a frame that
    // changed.
    if (t_ >= T_RING) {
        uint32_t into = t_ - T_RING;
        int full = c.text_width("Nova D1", 2, false);
        int x0   = cx - full / 2;
        int shown = (int)(into * full / (T_NAME - T_RING));
        if (shown > full) shown = full;
        c.text(x0, cy - 12, "Nova D1", 1, 2, false);
        // Mask off what has not arrived yet. Drawing the string and then
        // clearing the remainder is far cheaper than measuring a prefix each
        // frame, and it lands on exactly the same pixels.
        if (shown < full) c.fill_rect(x0 + shown, cy - 14, full - shown + 2, 18, 0);
    }

    if (t_ >= T_NAME) {
        uint32_t into = t_ - T_NAME;
        c.text_centred(cy + 8, "RPCortex", 1);
        // An underline that grows out from the centre, both ways at once.
        int w = (int)(into * 40 / (T_SIGN - T_NAME));
        if (w > 40) w = 40;
        c.hline(cx - w / 2, cy + 8 + FONT_H + 2, w, 1);
    }
}

}  // namespace nova
