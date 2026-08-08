// Desc: The Shell, Scripts and App Store screens — the three that run commands.
// File: novagui_apps.h
//
// A flat sibling of novagui, importing only the UI leaf. What these three have
// in common is that none of them does the work itself: the shell, the script
// runner and the package manager are all already in the firmware, and a screen
// that re-implemented any of them would be a second copy to keep in step.
#ifndef NOVA_GUI_APPS_H
#define NOVA_GUI_APPS_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_shell(void);
void open_scripts(void);
void open_store(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_APPS_H
