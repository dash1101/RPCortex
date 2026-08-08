// Desc: The Files browser, the Logs reader and the Alerts queue.
// File: novagui_files.h
//
// A flat sibling of novagui, the same shape as novagui_tools and
// novagui_system: it imports the UI leaf and the two stores it displays, and
// nothing from the runner beyond push().
//
// The three screens are together because they are the same screen three times —
// a list of lines somebody scrolls, with one destructive action behind a
// question. Splitting them would mean three copies of the windowing.
#ifndef NOVA_GUI_FILES_H
#define NOVA_GUI_FILES_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_files(void);
void open_logs(void);
void open_alerts(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_FILES_H
