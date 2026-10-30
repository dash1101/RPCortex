// Desc: Saved codes - where they live, how they are named, listed, renamed and deleted.
// File: novagui_codes.h
//
// The Sub-GHz, infrared and NFC apps all keep what they capture as files in a folder of
// their own under /nova/codes, and all want the same four things around them: a name a
// person typed turned into a filename that is safe, a list of what is saved, a way to
// rename and delete, and a way to write a file out piece by piece. That is this file.
// What a code IS - a .sub, a .ir, a .nfc - is the codecs' business, not this file's.
//
// ONE LIST AT A TIME. The list of saved names is a file-static, because a screen has 384
// bytes of pool and thirty-two names do not fit in it. Only one library is on screen at a
// time, and opening one scans afresh, so nothing is shared that could be stale.
#ifndef NOVA_GUI_CODES_H
#define NOVA_GUI_CODES_H

#include "novacanvas.h"
#include "novacodec.h"

namespace nova {
namespace screens {
namespace codes {

#define CODES_NAME_MAX 24       // a name, without its extension
#define CODES_MAX      32       // how many a list holds

struct Kind {
    const char *dir;            // "/nova/codes/subghz"
    const char *ext;            // ".sub"
    const char *title;          // "Sub-GHz"
};

extern const Kind kSubGhz;
extern const Kind kIr;
extern const Kind kNfc;

// Make the folder (and the one above it) if it is not there.
void ensure_dir(const Kind &k);

// What a person typed, made safe to be a filename: letters, digits, space, dash and
// underscore are kept, anything else becomes an underscore, the ends are trimmed and it
// is cut to fit. False when nothing is left.
bool clean_name(const char *typed, char *out, unsigned cap);

// "gate" if there is no gate.sub yet, else "gate-2", "gate-3", ...
void unique_name(const Kind &k, const char *base, char *out, unsigned cap);

// "/nova/codes/subghz/gate.sub"
void path_for(const Kind &k, const char *name, char *out, unsigned cap);

// --- writing a file in pieces -------------------------------------------------------------
//
// A Sink onto a file. The file is truncated when this is opened and written 192 bytes at a
// time through fw_file_append, so a long capture never has to exist whole in RAM. One at a
// time: the state is a static. file_sink_done() flushes what is left and says whether every
// write made it; a file that failed part way is removed rather than left half-written.
codec::Sink file_sink_open(const char *path);
bool        file_sink_done(void);

// --- the list ------------------------------------------------------------------------------

struct List {
    char names[CODES_MAX][CODES_NAME_MAX];
    int  n;
    int  sel;
    int  top;
    bool more;                  // there were more files than the list holds
};

List &list(void);

// Read the folder: files with the kind's extension, extension removed, in alphabetical
// order. The cursor is kept on the same name if it is still there.
void scan(const Kind &k);

void        move(int dir);                  // wraps
const char *selected(void);                 // the name under the cursor, or null when empty

// Draw the list from the top of the body: a row per name, the cursor inverted, a scroll
// bar when it does not all fit. `empty` is what to say when there is nothing saved.
void draw(Canvas &c, const char *empty);

// --- what can be done to a saved file -------------------------------------------------------

// Ask for a new name on the keyboard and rename the file. `done` runs after a rename that
// worked, with `ctx`. A name that is already taken is refused out loud.
void ask_rename(const Kind &k, const char *name, void (*done)(void *), void *ctx);

// Ask whether to delete it, and do so on a yes.
void ask_delete(const Kind &k, const char *name, void (*done)(void *), void *ctx);

}  // namespace codes
}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_CODES_H
