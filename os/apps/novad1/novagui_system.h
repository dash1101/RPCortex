// Desc: The System screens — what the hardware is doing, and the settings that change it.
// File: novagui_system.h
#ifndef NOVA_GUI_SYSTEM_H
#define NOVA_GUI_SYSTEM_H

namespace nova {
namespace screens {

void open_hardware(void);        // every module, what it needs, whether it answered
void open_display_settings(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_SYSTEM_H
