// Desc: The Shell, Scripts, App Store and Updates screens — the four that run commands.
// File: novagui_apps.h
//
// A flat sibling of novagui, importing only the UI leaf. What these four have
// in common is that none of them does the work itself: the shell, the script
// runner, the package manager and the firmware updater are all already in the
// firmware, and a screen that re-implemented any of them would be a second copy
// to keep in step.
//
// They also share ONE command runner and ONE capture buffer, which is why they
// are one file: the OS has a single output capture, so a second command started
// while the first holds it runs and hands back nothing. That invariant is
// enforced between these screens by construction, and splitting them would
// spread it across translation units where nothing checks it.
#ifndef NOVA_GUI_APPS_H
#define NOVA_GUI_APPS_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_shell(void);
void open_scripts(void);
void open_store(void);
void open_updates(void);

// --- the update flow, shared with `novad1 selfupdate` -------------------------
//
// A package cannot install itself, so both routes stage the install on a
// maintenance boot instead. The pieces are here rather than duplicated because
// the shell command and the screen have to agree about the script, where it
// lives and what would stop it running.

// Given `autonomy status` and `whoami`, will a maintenance boot come up without
// a login prompt AS AN ADMIN? The staged lines want one, and a boot that stops
// at a prompt — or comes up as somebody who cannot even run `reboot` — is a
// device left in maintenance mode with a frozen panel. The gate on the whole
// flow, and the reason it needs both answers.
bool update_autonomy_ok(const char *status, const char *whoami);

// Write the two-line script the maintenance boot runs. False when it could not
// be written, which is a full or broken filesystem and not worth guessing at.
bool update_write_script(void);

// The one command that stages it: `safeboot script <path>`. Whoever runs it
// does not come back — safeboot restarts from inside the call.
const char *update_stage_line(void);

// Did the update staged last time land? Reads two registry keys, and only pays
// for anything when they are set. Called once from gui::begin().
void update_report_start(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_APPS_H
