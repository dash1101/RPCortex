// Desc: The boot animation.
// File: novasplash.h
//
// Plays once, over the real boot work, and pops when it is done. It must never
// ADD boot time — the device is doing something useful behind it, and a splash
// that makes the thing slower to use is decoration charging rent.
#ifndef NOVA_SPLASH_H
#define NOVA_SPLASH_H

#include "novaui.h"

namespace nova {

class SplashScreen : public ui::Screen {
public:
    void enter(void) override;
    void draw(Canvas &c) override;
    bool tick(uint32_t dt_ms) override;
    ui::Action on_event(Event e) override;
    bool fullscreen(void) const override { return true; }
    bool animating(void) const override { return true; }

private:
    uint32_t t_;
};

}  // namespace nova

#endif  // NOVA_SPLASH_H
