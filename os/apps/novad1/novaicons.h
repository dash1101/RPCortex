// Desc: The app icons — drawn, not stored.
// File: novaicons.h
//
// Every icon is procedure rather than a bitmap, which is how the MicroPython
// suite did it and is worth keeping for one reason: the home screen draws the
// centre icon large and its neighbours small, and a bitmap would need a copy at
// each size or would look like a bitmap scaled up. A circle drawn at radius 6
// and radius 12 is a circle both times.
//
// An unknown key falls back to a box with the app's initial in it, so a new app
// always has SOMETHING recognisable rather than a hole where an icon should be.
#ifndef NOVA_ICONS_H
#define NOVA_ICONS_H

#include "novacanvas.h"

namespace nova {
namespace icons {

// Draw `key` centred on (cx, cy) at about `r` pixels of radius. Icons were drawn
// against r = 12 for the gallery centre and r = 6 for its neighbours, which is
// the pair the port was checked at and the pair tools/icondiff.py still compares.
// The gallery now asks for ui::ICON_SMALL, a pixel under that; anything added
// here has to stay legible a size below the one it was drawn at.
void draw(Canvas &c, const char *key, int cx, int cy, int r, const char *label_fallback);

}  // namespace icons
}  // namespace nova

#endif  // NOVA_ICONS_H
