// Desc: Third-party apps — finding them on disk, and reading what they say.
// File: novaapps.h
//
// A Nova D1 app written by somebody else is ONE TEXT FILE and nothing else. It
// is never executed; it is read. That is the whole security story, and it is
// the reason this shape was chosen over the obvious one.
//
// The obvious one — a third-party PACKAGE that Nova D1 calls to draw a screen —
// cannot work. sandbox_enter keeps one package call per task, and a Nova D1
// screen is already inside one: the GUI task is in Nova D1's sandbox, and so is
// the job task every screen spawns to run a command. So a package command
// dispatched from any Nova D1 screen is refused by app_run_owner, correctly.
// The MicroPython suite got away with `exec`ing a downloaded module and said so
// out loud in its own docs; there is no eval here and no honest equivalent.
//
// What is left is very nearly as much. The FIRMWARE command surface is
// reachable — cc1101, sx1276, bt, nfc, ibutton, net, fetch, reg, script, sd, pkg
// — and that is the radios, the contact readers, the network and the
// filesystem. What is out of reach is the peripherals that happen to be
// packaged: gpio, i2cscan, dht, ws2812.
//
// This file is the LEAF of it: discovery and parsing, no screen and no command
// runner, so every decision it makes can be checked on a host.
#ifndef NOVA_APPS_H
#define NOVA_APPS_H

#include "novamodtab.h"          // Category

namespace nova {
namespace napps {

// Where they live, and what they are called. `.napp` rather than `.app`
// because a `.app` in this tree is a compiled package image, and one extension
// meaning two things is a support question waiting to happen.
#define NOVA_APPS_DIR "/nova/apps"
#define NOVA_APP_EXT  ".napp"

// Eight apps and twelve rows each. Both are the same kind of number as
// SCRIPT_FILES_MAX: large enough that nobody meets it in ordinary use, small
// enough that the tables live in bss without anybody having to think about it.
#define NAPP_MAX       8
#define NAPP_ROWS_MAX  12

// One app's whole file, resident only while its screen is up.
//
// A name, five header lines and a dozen rows is about seven hundred bytes. The
// rest is for COMMENTS, and the number was set by measuring rather than
// guessed: the shipped example is mostly an explanation of the format, and an
// author who copies it and fills every row it can hold must not hit a ceiling.
// novaapps_test pins exactly that sum. A file past this is refused BY SIZE,
// rather than read as far as it fitted and quietly losing the author's last
// rows.
#define NAPP_TEXT_MAX  2048

// How far into a file the scan looks for the app.* header.
//
// The scan reads eight of these every time the home rescans, and it only wants
// the name, the version and the category — so it reads a window rather than the
// whole file. The header has to be inside it. A file that buries app.name under
// six hundred bytes of comment is listed under its FILE NAME until it is
// opened, and opening it re-reads the header from the whole thing.
#define NAPP_HEAD_MAX  512

// Why an app will not open.
//
// A faulty app is LISTED, not hidden. The MicroPython suite dropped an app that
// would not compile and said nothing, which left somebody with a file on the
// device, no icon, and no way to find out why. The same judgement the module
// table already makes: say what is wrong where the person is looking.
enum NappFault {
    NAPP_OK = 0,
    NAPP_NO_NAME,        // no app.name
    NAPP_NO_ROWS,        // nothing to do
    NAPP_KIND,           // a kind this build does not interpret — kind: py
    NAPP_UNREADABLE,     // the file went away between the scan and the open
    NAPP_TOO_BIG         // larger than NAPP_TEXT_MAX
};

// What to put on the panel for a fault. One short sentence, sized for a
// 21-column line after wrapping.
const char *fault_text(NappFault f);

// The catalogue key of an installed app is its file's stem behind this.
//
// A PREFIX rather than the bare stem, so an app called cc1101.napp cannot land
// a second row in the catalogue under a key a built-in already owns — where
// `novad1 apps hide cc1101` would hit both of them and the icon map would hand
// the newcomer the radio glyph. The MicroPython suite prefixed for a related
// reason and it was the right instinct.
#define NAPP_KEY_PREFIX "app_"

// One app, as the catalogue sees it.
struct NappItem {
    char      key[20];        // NAPP_KEY_PREFIX and the file's stem
    char      label[15];      // app.name, trimmed to fit an icon caption
    char      file[40];       // the name inside NOVA_APPS_DIR
    char      ver[12];        // app.ver, for `novad1 apps`
    Category  cat;
    NappFault fault;          // what the HEADER already knows is wrong
};

// --- discovery ----------------------------------------------------------------

// Re-read NOVA_APPS_DIR and every app's header. Returns how many were found.
//
// Safe to call from either task. A scan already running is not started a second
// time — the buffer it reads each header into is one static, because half a
// kilobyte of a package's three-kilobyte stack is too much to spend inside the
// runner's event loop.
//
// What it is NOT safe to do from the shell task is rebuild the CATALOGUE from
// the result; that rewrites the array a Gallery is drawing out of. gui marks
// itself dirty and does that part on its own task.
int scan(void);

int count(void);
const NappItem *at(int i);

// Find by catalogue key, or null.
const NappItem *by_key(const char *key);

// Something changed on disk — an install, a delete, a file dropped over USB.
//
// Set from the shell task and read from the GUI task, which is safe because
// both run the same image and the flag is one bool. The MicroPython suite had
// no equivalent, so a freshly installed app appeared only after some unrelated
// rebuild while the screen said "Installed (on home)!".
//
// It starts SET, so the first ask always looks at the disk. Nothing has scanned
// on a device where the runner has never been started, and "no apps" and "not
// looked yet" are not the same answer.
void mark_dirty(void);

// Rescan if anything said so. TRUE when it did, so the caller knows to rebuild
// whatever it derived from the last scan.
bool rescan_if_dirty(void);

// --- one app's rows -------------------------------------------------------------

struct NappRow {
    const char *label;
    const char *action;       // a shell command line
};

// Read `it`'s file and parse it. Returns the row count; `why` says what went
// wrong when that is zero.
//
// The rows point INTO a static buffer that the next call overwrites, so exactly
// one app is loaded at a time. That is the same bargain the App Store screen
// makes with its own output buffer, for the same reason: a fixed table of
// twelve labels and twelve command lines costs more than the text they came
// from.
int load(const NappItem &it, NappFault *why);

int count_rows(void);
const NappRow *row(int i);

// --- the parsers, exposed because they are the part worth testing ----------------

// The app.* keys, from as much of the file as is to hand. Fills label, ver, cat
// and fault; leaves key and file alone, because those come from the listing.
void parse_header(const char *text, NappItem *out);

// The `Label = action` lines. SPLITS `text` IN PLACE, so the rows point into
// it and it has to outlive them.
int parse_rows(char *text, NappRow *out, int max);

// "Wireless" -> CAT_WIRELESS, case-insensitively. Anything unrecognised, and
// anything empty, is CAT_TOOLS — which is where the MicroPython suite put an
// app it could not place, and it is the right default: a tool is what an app
// somebody wrote is, until it says otherwise.
Category category_from(const char *name);

// The file name at the end of a URL, if it is one this may write.
//
// The ONLY thing between a URL and a write to the filesystem, so it refuses
// rather than repairs: a segment carrying a slash, starting with a dot, holding
// anything outside [A-Za-z0-9._-], longer than the field, or not ending in
// NOVA_APP_EXT. A query string is not part of the name.
//
// Here rather than beside the command that uses it because it is pure string
// work guarding a write, which is exactly the kind of thing that belongs where
// a host test can reach it.
bool url_filename(const char *url, char *out, unsigned cap);

}  // namespace napps
}  // namespace nova

#endif  // NOVA_APPS_H
