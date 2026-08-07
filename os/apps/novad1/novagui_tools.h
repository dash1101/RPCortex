// Desc: The Tools and Sensors screens that need no wired module.
// File: novagui_tools.h
//
// A flat sibling of novagui, importing only the UI leaf. The MicroPython suite
// grew a 2,900-line novagui.py and spent months pulling it apart again; the
// lesson is to write a screen as a sibling from the start rather than adding it
// to the runner and splitting it out later.
#ifndef NOVA_GUI_TOOLS_H
#define NOVA_GUI_TOOLS_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_resources(void);
void open_clock(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_TOOLS_H
