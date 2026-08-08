// Desc: The Task Manager — what is running now, and what runs at every boot.
// File: novagui_tasks.h
//
// A flat sibling of novagui, the same shape as novagui_tools: it imports the UI
// leaf and nothing above it, so the screens can move between files as the suite
// grows.
//
// One entry point for two lists that people confuse with each other, and the
// whole reason they are behind a single door is so the device can keep saying
// which is which. Stopping a task ends when the board next boots. Removing a
// service is still true after it.
#ifndef NOVA_GUI_TASKS_H
#define NOVA_GUI_TASKS_H

namespace nova {
namespace screens {

// Pushes the Task Manager and returns. The App table's open functions all have
// this shape, so this one does too.
void open_tasks(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_TASKS_H
