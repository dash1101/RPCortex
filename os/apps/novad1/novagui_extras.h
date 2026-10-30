// Desc: The small apps - dice, counter, timer, base converter, torch and one-time codes.
// File: novagui_extras.h
//
// The MicroPython suite shipped these as kind:py apps in its app store. Vela II cannot run
// a package's code inside another package, so they are screens of the Nova D1 package
// itself, in the Tools folder, where the home settings can hide any of them.
#ifndef NOVA_GUI_EXTRAS_H
#define NOVA_GUI_EXTRAS_H

namespace nova {
namespace screens {

void open_dice(void);
void open_counter(void);
void open_timer(void);
void open_baseconv(void);
void open_torch(void);
void open_totp(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_EXTRAS_H
