// Desc: The startup check — what the device found, while it was finding it.
// File: novabootcheck.h
//
// The screen after the splash. It is not decoration and it is not a progress bar
// over nothing: each step is a real check, and the result of each one is what
// the rest of the session then relies on.
//
// Steps run ONE PER FRAME rather than all at once behind a spinner. Two reasons:
// the bar moves because work finished rather than because time passed, so it is
// telling the truth; and a step that blocks — an I2C probe on a bus with nothing
// on it, a network query — does not freeze the animation, because the animation
// is between steps rather than around them.
#ifndef NOVA_BOOTCHECK_H
#define NOVA_BOOTCHECK_H

#include "novaui.h"

namespace nova {

class BootCheckScreen : public ui::Screen {
public:
    void enter(void) override;
    void draw(Canvas &c) override;
    bool tick(uint32_t dt_ms) override;
    ui::Action on_event(Event e) override;
    bool fullscreen(void) const override { return true; }
    bool animating(void) const override { return !done_; }

    // Public because the row names live in the .cpp beside the code that fills
    // them in, and an array sized from a private constant cannot be declared
    // there. Keeping the two next to each other is worth more than the access
    // control on a count.
    static constexpr int STEPS = 9;

    // The opening animation, in milliseconds. Slower than the first attempt,
    // which ran the whole sequence in 1.8 s and read as a flicker.
    static constexpr uint32_t RING_MS  = 700;
    static constexpr uint32_t NAME_MS  = 1500;
    static constexpr uint32_t INTRO_MS = 2100;

private:
    void draw_intro(Canvas &c);
    uint32_t intro_;
    int      step_;
    bool     done_;
    uint32_t hold_;      // how long the finished list has been shown
    uint32_t spin_;

    // What each check found, so the list can be redrawn without redoing them.
    // Indexed by step number.
    int8_t   result_[STEPS];      // -1 not run, 0 bad, 1 good, 2 skipped
    char     detail_[STEPS][22];

    void run_step(int i);
};

}  // namespace nova

#endif  // NOVA_BOOTCHECK_H
