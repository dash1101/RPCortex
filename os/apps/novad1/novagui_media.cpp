// Desc: The media player — a WAV, a folder of them, and a Bluetooth speaker.
// File: novagui_media.cpp
//
// THERE IS NO AUDIO CALL IN THE PACKAGE ABI. What exists is the firmware's own
// `btaudio` command, run through fw_shell_run, which hands back the text it
// printed — the same arrangement the BLE screens have with `bt`:
//
//     btaudio status              what is connected, what is playing
//     btaudio connect <address>   pair with a speaker or headphones
//     btaudio play <file.wav>     stream it
//     btaudio stop
//     btaudio volume <0-100>
//     bt scan classic <seconds>   find a speaker to connect to
//
// Four things follow from that, and every screen here is shaped by them.
//
//   * THERE IS NO PAUSE. `btaudio stop` pauses the stream AND ends the feeder
//     task, which closes the file and loses the position; a later `play` starts
//     at byte zero. So the middle transport control is PLAY / STOP and is
//     labelled that way. A control that looked like a pause and silently
//     restarted the track would be the exact silent failure this suite spends
//     its comments on.
//   * WAV HAS NO ID3. The header carries a sample rate, a channel count and a
//     length, and a RIFF INFO chunk MAY carry a title and an artist. Whatever is
//     actually in the file is shown and nothing else — no artist is invented
//     from a filename, and a file with no INFO says so by showing its format
//     line instead.
//   * `btaudio play` READS ONLY THE FIRST 256 BYTES to decide whether a file is
//     playable, so this reads the same 256 and reaches the same verdict. Judging
//     a file over a larger window would mean offering tracks the player then
//     refuses, which is worse than being told up front.
//   * connect BLOCKS for up to ten seconds and a classic scan for about twelve,
//     inside a wait no flag can reach. Both run on a task and the screen only
//     ever looks at the result — see the worker below.
//
// DEVICE-UNCONFIRMED: no speaker has been connected to any of this. The browse,
// queue, metadata and command-building halves are host-tested; the A2DP half is
// btaudio's, and it says the same about itself.
#include "novagui_media.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// NO DEFAULT INITIALISERS anywhere in this file, and every bulky thing is a
// file-static. Screens are placement-constructed into a zeroed 384-byte slot and
// a default member initialiser would emit .init_array, which nothing in a
// package ever runs; the queue, the path stack and the capture buffer would not
// fit in a slot in any case.

// Long enough for anything littlefs holds here. A path that would not fit is
// REFUSED rather than truncated, because a truncated path is a different path
// that may well exist.
#define MEDIA_PATH_MAX 128

// How deep the browser goes. Four levels below the music root is more nesting
// than a folder of tracks ever has, and each level costs a path buffer.
#define MEDIA_LEVEL_MAX 4

// Where the browser starts, unless the registry says otherwise.
#define MEDIA_KEY_ROOT    NOVA_KEY_PREFIX "Music"
#define MEDIA_KEY_SPEAKER NOVA_KEY_PREFIX "Speaker"
#define MEDIA_ROOT_DEF    "/nova/music"

// The spinner step every screen in the suite turns at.
#define MEDIA_SPIN_MS 140

// --- what the last `btaudio status` said ------------------------------------------
//
// One picture of the player, shared by every screen here, because there is one
// player. UNKNOWN is a real state and is drawn as "--": before the first reply
// this package has been told nothing, and a confident "stopped" would be a guess
// rendered as a fact.

enum { MEDIA_UNKNOWN = 0, MEDIA_IDLE, MEDIA_PLAYING };

static uint8_t  g_media_state;
static bool     g_media_linked;             // a speaker is connected
static char     g_media_peer[20];           // its address, when it is
static char     g_media_now[MEDIA_PATH_MAX];// what btaudio says it is playing
static int      g_media_vol;                // 0-100, or -1 for never told
static int      g_media_prog;               // 0-100, or -1 when not streaming
static uint32_t g_media_under;              // underruns, i.e. audible stutter
static bool     g_media_known;              // a status reply has landed at all

// --- the worker -------------------------------------------------------------------
//
// A GENERATION COUNTER, not a cancel flag. Neither `btaudio connect` nor
// `bt scan classic` can be interrupted — both sit in a wait loop inside the
// firmware for ten or twelve seconds with nowhere for a flag to be read — so
// what a screen change can do is make the answer irrelevant. The task compares
// the generation it started with against the current one AFTER the call returns
// and before it writes anything, and a job that outlived its screen throws its
// reply away rather than dropping it into a buffer the next screen is using.
//
// The task is never killed, for the same reason: it is inside the radio driver
// holding the shared WiFi/Bluetooth lock, and killing it there would leave that
// lock held for good.
//
// Plain volatile is enough. These cores have no data cache over SRAM, so a write
// on one is visible to the other; the ordering that matters is that g_media_busy
// clears LAST, after g_media_ready is set.

// One kilobyte, sized by the LONGEST thing that lands in it: a classic inquiry,
// whose rows are about fifty characters each and of which the firmware keeps up
// to twenty-four. A status reply is a fifth of that. It is truncated rather than
// refused if a room is busier than that, and MEDIA_SCAN_MAX caps the list
// anyway — but a buffer sized for the status reply would have silently lost
// speakers, which is the failure that reads as "my speaker is not there".
#define MEDIA_OUT_BYTES 1024

static char              g_media_out[MEDIA_OUT_BYTES];
static char              g_media_cmd[RPC_SHELL_LINE_MAX];
static volatile uint32_t g_media_gen;
static volatile uint32_t g_media_req;
static volatile uint8_t  g_media_busy;
static volatile uint8_t  g_media_ready;

static int media_job_task(void *arg) {
    (void)arg;
    uint32_t mine = g_media_req;
    g_media_out[0] = 0;
    fw_shell_run(g_media_cmd, g_media_out, sizeof(g_media_out));
    if (mine == g_media_gen && !fw_task_should_stop()) {
        g_media_ready = 1;
    } else {
        g_media_out[0] = 0;             // nobody is waiting for this any more
    }
    g_media_busy = 0;
    gui::invalidate();
    return 0;
}

// Start `line` on a task. False when one is already running or the line is too
// long for the shell, both of which the caller has to say out loud rather than
// silently doing nothing.
static bool media_job_start(const char *line) {
    if (g_media_busy || g_media_ready) return false;
    if (!line || !line[0]) return false;
    unsigned n = 0;
    while (line[n]) n++;
    if (n + 1 > sizeof(g_media_cmd)) return false;
    nova::copy(g_media_cmd, sizeof(g_media_cmd), line);
    g_media_out[0] = 0;
    g_media_req  = g_media_gen;
    g_media_busy = 1;
    // TASK_STACK_MIN's worth is ample for what the PACKAGE does here — one ABI
    // call and a flag. btstack and the shell run on this stack too, and they are
    // carried by the firmware reserve the loader adds on top of what a package
    // asks for, exactly as the BLE scan relies on.
    if (fw_task_spawn("novamedia", media_job_task, nullptr, 2048) < 0) {
        g_media_busy = 0;
        return false;
    }
    return true;
}

// Is a reply waiting for whoever is on screen now?
static bool media_job_take(void) {
    if (!g_media_ready) return false;
    g_media_ready = 0;
    return true;
}

static bool media_job_running(void) { return g_media_busy != 0; }

// Nothing running AND nothing waiting to be read. The second half is what makes
// the buffer safe to borrow for a poll: a reply nobody has taken yet is still
// the answer to a question somebody asked.
static bool media_job_idle(void) { return !g_media_busy && !g_media_ready; }

// Anything in flight belongs to the screen that started it. Bumped on the way
// into and out of every screen here, so a reply that arrives late lands nowhere.
//
// It clears the pending refusal too. A press refused on a screen somebody has
// already left is not worth carrying to the next one, and reporting it there
// would be a message about something that is no longer on the panel.
static void media_job_disown(void);

// --- reading what btaudio printed ---------------------------------------------------
//
// Written against cmd_btaudio in os/shell/btaudio.cpp. Captured output carries
// the tag with the colour stripped, and the rows are two spaces, a key, column
// padding, then the value:
//
//     [:] Bluetooth audio
//       Name       RPCortex
//       Speaker    AA:BB:CC:DD:EE:FF
//       Playing    /nova/music/track.wav
//       Position   42%  (44100 Hz, 2 channels)
//       Volume     80%
//       Underruns  0
//
// Fields are taken BY KEY rather than by column, because the padding in those
// format strings is a minimum and an address wider than the column would push
// every fixed offset along by one.

// Digits, and nothing else. There is no atoi in the firmware's export table —
// a package gets memcpy, strlen, strcmp, strchr, strstr, snprintf and a handful
// more, and reaching for anything else fails at the link rather than at run
// time, which is the right place but only if it is expected.
static int media_num(const char *s) {
    if (!s) return -1;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 999999) return 999999;
        s++;
    }
    return v;
}

// The value of the "  Key   value" row, or false when the text has no such row.
static bool media_field(const char *text, const char *key, char *out, unsigned cap) {
    if (cap) out[0] = 0;
    const unsigned klen = (unsigned)strlen(key);
    for (const char *p = text; *p; ) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p) p++;

        while (line < end && *line == ' ') line++;
        if ((unsigned)(end - line) <= klen) continue;
        if (strncmp(line, key, klen) != 0) continue;
        const char *v = line + klen;
        // The key has to be a whole word: "Volume" must not match a row that
        // happened to begin "VolumeStep".
        if (*v != ' ') continue;
        while (v < end && *v == ' ') v++;
        // Trailing spaces are the format string's padding, not the value.
        while (end > v && end[-1] == ' ') end--;
        unsigned n = (unsigned)(end - v);
        if (n + 1 > cap) n = cap ? cap - 1 : 0;
        for (unsigned i = 0; i < n; i++) out[i] = v[i];
        if (cap) out[n] = 0;
        return true;
    }
    return false;
}

// Fold a `btaudio status` reply into the picture above.
//
// An EMPTY buffer is not an empty answer. There is one output capture in the OS
// and a command whose capture was already held still runs and prints nowhere, so
// nothing coming back means "somebody else had the buffer", which must not be
// rendered as "no speaker".
static void media_status_read(const char *text) {
    if (!text || !text[0]) return;
    char v[MEDIA_PATH_MAX];

    if (media_field(text, "Speaker", v, sizeof(v))) {
        g_media_linked = strcmp(v, "not connected") != 0;
        nova::copy(g_media_peer, sizeof(g_media_peer), g_media_linked ? v : "");
    }
    if (media_field(text, "Playing", v, sizeof(v))) {
        const bool playing = strcmp(v, "nothing") != 0;
        nova::copy(g_media_now, sizeof(g_media_now), playing ? v : "");
        g_media_state = playing ? MEDIA_PLAYING : MEDIA_IDLE;
        g_media_known = true;
    }
    if (media_field(text, "Volume", v, sizeof(v)))    g_media_vol   = media_num(v);
    if (media_field(text, "Underruns", v, sizeof(v))) g_media_under = (uint32_t)media_num(v);
    // Position is only printed while something is streaming, so its absence is
    // information: there is no progress to draw.
    g_media_prog = media_field(text, "Position", v, sizeof(v)) ? media_num(v) : -1;
}

// The status poll runs INLINE rather than on the worker, and it is the only
// btaudio call here that does.
//
// `btaudio status` prints six globals and touches no radio, no file and no lock
// that a command at the console does not already take — microseconds, once every
// MEDIA_POLL_MS. Putting it on the worker would mean the one job slot were
// permanently occupied by a poll, so a transport button pressed at the wrong
// moment would be refused, which is a worse failure than a poll that costs a
// hundred microseconds on the UI task.
//
// Everything that reaches the radio — connect, play, stop, scan — goes through
// the worker, because every one of those can sit in the Bluetooth lock.
#define MEDIA_POLL_MS 1500

// It borrows the WORKER'S buffer rather than keeping one of its own, which is
// six hundred bytes of a package that has twelve kilobytes of heap and every
// byte of bss counted against the load. Safe only because of the idle check:
// while a job is running the buffer is being written from the other task, and
// while a reply is unread it is still somebody's answer.
static void media_poll_now(void) {
    if (!media_job_idle()) return;
    g_media_out[0] = 0;
    fw_shell_run("btaudio status", g_media_out, sizeof(g_media_out));
    media_status_read(g_media_out);
}

// The first line of a reply worth showing somebody, with the tag taken off. A
// tag is for a terminal, not for a panel nine pixels tall.
static void media_first_line(const char *text, char *out, unsigned cap) {
    if (cap) out[0] = 0;
    if (!text) return;
    // The LAST non-empty line, the way the store screen picks its reason: a
    // command that got somewhere prints its heading first, and the heading is
    // the one line that explains nothing.
    const char *best = nullptr;
    for (const char *p = text; *p; ) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        if (e > p) best = p;
        p = *e ? e + 1 : e;
    }
    if (!best) return;
    if (best[0] == '[' && best[1] && best[2] == ']' && best[3] == ' ') best += 4;
    unsigned n = 0;
    while (best[n] && best[n] != '\n' && n + 1 < cap) n++;
    for (unsigned i = 0; i < n; i++) out[i] = best[i];
    if (cap) out[n] = 0;
}

// --- the WAV header -----------------------------------------------------------------
//
// A second parser rather than os/core/wav.cpp, because that one is firmware and
// is not on the ABI — a package cannot call it. It is kept deliberately close to
// it: the same chunk walk, the same even-length padding rule, the same refusal
// of anything that is not 16-bit PCM in one or two channels, so a file this
// screen calls playable is a file wav_parse will accept.
//
// What it adds is the LIST/INFO walk. INAM and IART are where a WAV keeps a
// title and an artist when it has them at all, and reading them is the whole
// difference between showing somebody their music and showing them a filename.

// The window btaudio itself reads. Matching it is the point — see the header.
#define MEDIA_HEAD_BYTES 256

enum {
    MEDIA_WAV_OK = 0,
    MEDIA_WAV_NONE,         // could not be read
    MEDIA_WAV_NOT_WAV,
    MEDIA_WAV_LONG,         // a real WAV whose header runs past what play reads
    MEDIA_WAV_FORMAT,       // not 16-bit PCM, mono or stereo
};

struct MediaWav {
    uint32_t rate;
    uint32_t data_bytes;
    uint16_t channels;
    uint16_t bits;
    char     name[22];      // INAM, if the file carries one in the window
    char     artist[22];    // IART, likewise
    uint8_t  why;           // MEDIA_WAV_*
};

static uint32_t media_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t media_rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

// Copy an INFO value, which is a zero-terminated string padded to an even
// length. Anything unprintable becomes a dot: the font substitutes for what it
// does not have, and a row of substitutes hides the text that was really there.
static void media_info_copy(char *out, unsigned cap, const uint8_t *p, uint32_t n) {
    unsigned w = 0;
    for (uint32_t i = 0; i < n && w + 1 < cap; i++) {
        const char ch = (char)p[i];
        if (!ch) break;
        out[w++] = (ch >= 0x20 && ch <= 0x7e) ? ch : '.';
    }
    // A value that is only padding is not a value.
    while (w && out[w - 1] == ' ') w--;
    out[w] = 0;
}

static void media_wav_info(const uint8_t *d, uint32_t len, MediaWav *out) {
    uint32_t pos = 4;                       // past "INFO"
    while (pos + 8 <= len) {
        const uint32_t size = media_rd32(d + pos + 4);
        const uint32_t body = pos + 8;
        if (body > len || size > len - body) return;
        if (!memcmp(d + pos, "INAM", 4))
            media_info_copy(out->name, sizeof(out->name), d + body, size);
        else if (!memcmp(d + pos, "IART", 4))
            media_info_copy(out->artist, sizeof(out->artist), d + body, size);
        pos = body + size + (size & 1);
    }
}

static void media_wav_parse(const uint8_t *d, uint32_t len, MediaWav *out) {
    memset(out, 0, sizeof(*out));
    out->why = MEDIA_WAV_NOT_WAV;
    if (!d || len < 12) return;
    if (memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return;

    // From here it IS a WAV, so a header that does not fit the window is a
    // different answer from a file that is not one.
    out->why = MEDIA_WAV_LONG;

    bool have_fmt = false;
    uint32_t pos = 12;
    while (pos + 8 <= len) {
        const uint32_t id   = pos;
        const uint32_t size = media_rd32(d + pos + 4);
        const uint32_t body = pos + 8;

        // A chunk claiming more than the window holds ends the walk. The data
        // chunk is the exception worth allowing: plenty of files are written
        // with a placeholder size and the samples really are there, which is the
        // same allowance wav_parse makes.
        if (body > len || size > len - body) {
            if (!memcmp(d + id, "data", 4) && have_fmt) {
                out->data_bytes = size;
                out->why = MEDIA_WAV_OK;
            }
            return;
        }

        if (!memcmp(d + id, "fmt ", 4)) {
            if (size < 16) return;
            const uint16_t format = media_rd16(d + body);
            out->channels = media_rd16(d + body + 2);
            out->rate     = media_rd32(d + body + 4);
            out->bits     = media_rd16(d + body + 14);
            // 1 is uncompressed PCM and 0xFFFE is the extensible form of it.
            // Anything else needs a decoder neither this nor btaudio has.
            if ((format != 1 && format != 0xFFFE) || out->bits != 16 ||
                out->channels < 1 || out->channels > 2 || out->rate == 0) {
                out->why = MEDIA_WAV_FORMAT;
                return;
            }
            have_fmt = true;
        } else if (!memcmp(d + id, "data", 4)) {
            if (!have_fmt) return;          // data before fmt is unreadable
            out->data_bytes = size;
            out->why = MEDIA_WAV_OK;
            return;
        } else if (!memcmp(d + id, "LIST", 4) && size >= 4 &&
                   !memcmp(d + body, "INFO", 4)) {
            media_wav_info(d + body, size, out);
        }

        // Chunks are padded to an even length and the pad byte is not counted in
        // the size. Missing that walks half a byte out of step for the rest of
        // the file.
        pos = body + size + (size & 1);
    }
}

// Read a file's header and describe it. The 256 bytes are btaudio's window.
static void media_wav_read(const char *path, MediaWav *out) {
    uint8_t head[MEDIA_HEAD_BYTES];
    const uint32_t got = fw_file_read_at(path, 0, head, sizeof(head));
    if (!got) { memset(out, 0, sizeof(*out)); out->why = MEDIA_WAV_NONE; return; }
    media_wav_parse(head, got, out);
}

static const char *media_wav_why(const MediaWav &w) {
    switch (w.why) {
        case MEDIA_WAV_OK:     return "";
        case MEDIA_WAV_NONE:   return "cannot be read";
        case MEDIA_WAV_LONG:   return "header past 256 bytes";
        case MEDIA_WAV_FORMAT: return "needs 16-bit PCM";
        default:               return "not a WAV file";
    }
}

// Seconds of audio, or 0 when the header does not say. Integer throughout: the
// package has no libm worth pulling in and none of this needs one.
static uint32_t media_seconds(const MediaWav &w) {
    const uint32_t per_sec = w.rate * w.channels * 2u;
    if (!per_sec || !w.data_bytes) return 0;
    return w.data_bytes / per_sec;
}

// "3:07". A track over an hour is shown in minutes rather than growing a field
// every screen would have to leave room for.
static void media_mmss(char *out, unsigned cap, uint32_t secs) {
    snprintf(out, cap, "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

// "44.1k stereo 3:07" — what is known about a file when nothing named it.
static void media_format_line(char *out, unsigned cap, const MediaWav &w) {
    if (w.why != MEDIA_WAV_OK) { snprintf(out, cap, "%s", media_wav_why(w)); return; }
    char dur[10];
    media_mmss(dur, sizeof(dur), media_seconds(w));
    snprintf(out, cap, "%u.%uk %s %s",
             (unsigned)(w.rate / 1000u), (unsigned)((w.rate % 1000u) / 100u),
             w.channels == 1 ? "mono" : "stereo", dur);
}

// --- names and paths -------------------------------------------------------------

// The part after the last separator. Written out rather than strrchr, which is
// not one of the string functions the firmware exports.
static const char *media_base(const char *path) {
    const char *found = path;
    for (const char *s = path; *s; s++) if (*s == '/' && s[1]) found = s + 1;
    return found;
}

// Build "<dir>/<name>", false when it would not fit. Not a bare snprintf,
// because snprintf truncates and a truncated path is a different path.
static bool media_join(char *out, unsigned cap, const char *dir, const char *name) {
    const int n = (dir[0] == '/' && !dir[1]) ? snprintf(out, cap, "/%s", name)
                                             : snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (unsigned)n < cap;
}

static bool media_is_wav(const char *name) {
    const char *dot = nullptr;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    return dot && nova::ieq(dot, ".wav");
}

// The name without its extension, which is the best title a file with no INFO
// chunk has.
static void media_stem(char *out, unsigned cap, const char *name) {
    unsigned n = 0;
    while (name[n] && n + 1 < cap) n++;
    while (n > 1 && name[n - 1] != '.') n--;
    if (n > 1) n--; else while (name[n] && n + 1 < cap) n++;
    for (unsigned i = 0; i < n; i++) out[i] = name[i];
    out[n] = 0;
}

// A path the shell can be handed. `btaudio play` takes one argument, so a name
// with a space in it has to be quoted — cmdline_split_args understands double
// quotes, and nothing else. A path CONTAINING a quote has no encoding at all in
// that tokeniser, so it is refused here rather than mis-run.
static bool media_play_line(char *out, unsigned cap, const char *path) {
    if (strchr(path, '"')) return false;
    const int n = snprintf(out, cap, "btaudio play \"%s\"", path);
    return n > 0 && (unsigned)n < cap;
}

// --- the queue --------------------------------------------------------------------
//
// A FOLDER IS THE QUEUE. next and prev walk the .wav files of whichever folder
// the current track came from, in the order the filesystem lists them.
//
// What is stored is the DIRECTORY INDEX of each track rather than its name: a
// name is up to 64 bytes and sixty-four of them is a third of what this whole
// package has to spare, where an index is two. The name is read back through
// fw_dir_entry at the moment it is needed, which is also what the file browser
// does and for the same reason — a row read when it is used cannot be stale.

#define MEDIA_Q_MAX 64

static char     g_media_qdir[MEDIA_PATH_MAX];
static uint16_t g_media_q[MEDIA_Q_MAX];
static int      g_media_qn;
static int      g_media_pos;                // -1 when nothing has been chosen
static bool     g_media_qfull;              // the folder held more than fits

// Scan `dir` for playable-looking files. Returns how many were found.
//
// The EXTENSION only; the header is not read here. Sixty-four header reads to
// open a folder is a visible pause on flash, and a .wav that turns out not to be
// one is caught the moment it is played, by btaudio, in its own words.
static int media_queue_build(const char *dir) {
    g_media_qn = 0;
    g_media_pos = -1;
    g_media_qfull = false;
    nova::copy(g_media_qdir, sizeof(g_media_qdir), dir);

    const int n = fw_dir_count(dir);
    FwDirEntry e;
    for (int i = 0; i < n; i++) {
        if (fw_dir_entry(dir, (unsigned)i, &e) != 1) break;
        if (e.is_dir || !media_is_wav(e.name)) continue;
        if (g_media_qn >= MEDIA_Q_MAX) { g_media_qfull = true; break; }
        g_media_q[g_media_qn++] = (uint16_t)i;
    }
    return g_media_qn;
}

static bool media_track_name(int pos, char *out, unsigned cap) {
    if (cap) out[0] = 0;
    if (pos < 0 || pos >= g_media_qn) return false;
    FwDirEntry e;
    if (fw_dir_entry(g_media_qdir, g_media_q[pos], &e) != 1) return false;
    nova::copy(out, cap, e.name);
    return true;
}

static bool media_track_path(int pos, char *out, unsigned cap) {
    char name[FW_NAME_MAX];
    if (!media_track_name(pos, name, sizeof(name))) return false;
    return media_join(out, cap, g_media_qdir, name);
}

// --- what the player is asked to do -------------------------------------------------
//
// The three commands that reach the radio, all of them through the worker, and
// all of them leaving a note behind for the screen to show when the reply lands.

static char g_media_note[64];       // the last thing btaudio said, in its words
static bool g_media_expect;         // a reply is wanted, rather than disowned
static uint8_t g_media_want;        // which command is out

enum { MEDIA_W_NONE = 0, MEDIA_W_PLAY, MEDIA_W_STOP, MEDIA_W_CONNECT, MEDIA_W_SCAN };

// Why something did not happen. A press that does nothing at all is what a
// broken button looks like on a device whose only input is one knob, so every
// refusal has a reason and every reason has words.
enum { MEDIA_R_OK = 0, MEDIA_R_NOTRACK, MEDIA_R_PATH, MEDIA_R_BUSY };

// Set by whoever refused, reported by whichever screen ticks next. Reported from
// a tick rather than from the press because ui::notice PUSHES, and pushing from
// inside a handler that is also about to push puts the two in the wrong order.
static uint8_t g_media_fail;

static void media_job_disown(void) {
    g_media_gen++;
    g_media_ready = 0;
    g_media_fail  = MEDIA_R_OK;
}

static const char *media_fail_text(uint8_t r) {
    switch (r) {
        case MEDIA_R_NOTRACK: return "No track chosen. Browse for one first.";
        case MEDIA_R_PATH:    return "That name is too long, or has a quote in it, "
                                     "and the player takes one word.";
        default:              return "The radio is still busy with the last thing "
                                     "it was asked. Try again in a moment.";
    }
}

// Report a pending refusal, once. True when something was said.
static bool media_report_fail(void) {
    if (!g_media_fail) return false;
    const uint8_t r = g_media_fail;
    g_media_fail = MEDIA_R_OK;
    ui::notice("Media", media_fail_text(r));
    return true;
}

static bool media_send(uint8_t what, const char *line) {
    if (!media_job_start(line)) { g_media_fail = MEDIA_R_BUSY; return false; }
    g_media_want   = what;
    g_media_expect = true;
    return true;
}

// Start the track at `pos`, if there is one there.
static bool media_play_at(int pos) {
    if (pos < 0 || pos >= g_media_qn) { g_media_fail = MEDIA_R_NOTRACK; return false; }
    char path[MEDIA_PATH_MAX], line[RPC_SHELL_LINE_MAX];
    if (!media_track_path(pos, path, sizeof(path)) ||
        !media_play_line(line, sizeof(line), path)) {
        g_media_fail = MEDIA_R_PATH;
        return false;
    }
    if (!media_send(MEDIA_W_PLAY, line)) return false;
    g_media_pos = pos;
    return true;
}

// --- Now Playing ----------------------------------------------------------------------
//
// The transport, drawn as four cells across the foot of the panel. Turning moves
// between them and SELECT works the one under the cursor, which is the whole
// control vocabulary this device has: one encoder, and BACK and HOME are spoken
// for everywhere else in the suite and must not be borrowed here.

enum { MEDIA_C_PREV = 0, MEDIA_C_PLAY, MEDIA_C_NEXT, MEDIA_C_VOL, MEDIA_C_COUNT };

// A triangle, apex left or apex right, `h` tall. The transport glyphs are built
// from these two and a bar, which keeps them the same weight as each other.
static void media_tri(Canvas &c, int x, int cy, int h, bool right, int col) {
    const int half = h / 2;
    for (int i = 0; i <= half; i++) {
        const int cx = right ? x + half - i : x + i;
        c.vline(cx, cy - i, 2 * i + 1, col);
    }
}

// How wide each glyph draws, so a cell can centre the one it holds. Written out
// rather than measured, because these are four hand-drawn shapes and there is
// nothing to measure them with.
static int media_glyph_w(int control) {
    switch (control) {
        case MEDIA_C_PREV: return 8;
        case MEDIA_C_PLAY: return g_media_state == MEDIA_PLAYING ? 9 : 5;
        case MEDIA_C_NEXT: return 8;
        default:           return 12;
    }
}

static void media_glyph(Canvas &c, int cell_x, int cy, int control, int col) {
    const int h = 9, half = h / 2;
    switch (control) {
        case MEDIA_C_PREV:                          // |< to the start of the last
            c.fill_rect(cell_x, cy - half, 2, h, col);
            media_tri(c, cell_x + 3, cy, h, false, col);
            break;
        case MEDIA_C_PLAY:
            // The one control whose SHAPE is the state. A square is stop and a
            // triangle is play, and there is no third glyph because there is no
            // pause to draw.
            if (g_media_state == MEDIA_PLAYING) c.fill_rect(cell_x, cy - half, h, h, col);
            else                                media_tri(c, cell_x, cy, h, true, col);
            break;
        case MEDIA_C_NEXT:                          // >| to the start of the next
            media_tri(c, cell_x, cy, h, true, col);
            c.fill_rect(cell_x + half + 2, cy - half, 2, h, col);
            break;
        default:                                    // a speaker, with its waves
            c.fill_rect(cell_x, cy - 2, 2, 5, col);
            media_tri(c, cell_x + 2, cy, h, false, col);
            c.vline(cell_x + half + 5, cy - 1, 3, col);
            c.vline(cell_x + half + 7, cy - 3, 7, col);
            break;
    }
}

class MediaNowScreen : public Screen {
public:
    // EVERY member, every time. Not because the slot is dirty — push<T> value-
    // initialises, so it arrives zeroed, and novagui_test checks that — but
    // because zero is not the right starting value for most of these, and a
    // field this forgets is a field silently starting at zero instead.
    void begin(void) {
        sel_ = MEDIA_C_PLAY;
        vol_mode_ = false;
        resume_after_stop_ = false;
        vol_send_ = 0;
        phase_ = 0;
        poll_ = MEDIA_POLL_MS;          // ask straight away rather than in a second
        grace_ = 0;
        meta_pos_ = -2;                 // nothing read yet, and -1 is a real answer
        refresh_meta();
    }

    const char *title(void) const override { return "Now Playing"; }

    int help(const char **out, int max) const override {
        if (max < 5) return 0;
        out[0] = "Turn to pick a control,";
        out[1] = "SELECT to work it.";
        out[2] = "There is no pause: STOP";
        out[3] = "loses the position.";
        out[4] = "Hold SELECT to stop.";
        return 5;
    }

    void enter(void) override { media_job_disown(); refresh_meta(); }
    void leave(void) override { flush_volume(); media_job_disown(); }

    bool animating(void) const override { return media_job_running(); }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool redraw = false;

        if (grace_ > dt) grace_ -= dt; else grace_ = 0;

        if (media_job_take()) {
            finished();
            redraw = true;
        }
        if (media_report_fail()) return true;

        // A volume change is sent once the knob stops rather than on every
        // detent. A detent is 5%, a spin is a dozen of them, and a dozen shell
        // commands queued behind one job slot is a control that lags a second
        // behind the hand turning it.
        if (vol_send_) {
            if (vol_send_ > dt) vol_send_ -= dt; else vol_send_ = 0;
            if (!vol_send_) { flush_volume(); redraw = true; }
        }

        poll_ += dt;
        if (poll_ >= MEDIA_POLL_MS && !media_job_running()) {
            poll_ = 0;
            const uint8_t was = g_media_state;
            media_poll_now();
            if (was == MEDIA_PLAYING && g_media_state == MEDIA_IDLE && !grace_)
                track_ended();
            redraw = true;
        }
        if (media_job_running()) redraw = true;      // the spinner is turning
        return redraw;
    }

    void draw(Canvas &c) override {
        char line[40];

        // What is playing, by the best name there is for it: the file's own INAM
        // when it has one, and the filename without its extension when it does
        // not. Never a guess dressed as a tag.
        if (meta_.name[0]) c.text_fit(0, ui::TOP, meta_.name, 1, c.width(), true);
        else if (stem_[0]) c.text_fit(0, ui::TOP, stem_, 1, c.width(), true);
        else               c.text(0, ui::TOP, "Nothing chosen", 1);

        // The artist ONLY when the file named one. Otherwise the format line,
        // which is what is actually known about the track.
        const int y2 = ui::TOP + ui::ROWH;
        if (meta_.artist[0]) {
            c.text_fit(0, y2, meta_.artist, 1, c.width(), true);
        } else if (stem_[0]) {
            media_format_line(line, sizeof(line), meta_);
            c.text_fit(0, y2, line, 1, c.width(), true);
        }

        // Position in the queue on the left, volume on the right, and the state
        // between them when there is room for it.
        const int y3 = ui::TOP + 2 * ui::ROWH;
        if (g_media_qn && g_media_pos >= 0) {
            snprintf(line, sizeof(line), "%d/%d", g_media_pos + 1, g_media_qn);
            c.text(0, y3, line, 1);
        }
        if (g_media_vol >= 0) snprintf(line, sizeof(line), "%d%%", g_media_vol);
        else                  snprintf(line, sizeof(line), "--");
        c.text(c.width() - c.text_width(line, 1, false), y3, line, 1);
        c.text_centred(y3, state_word(), 1);

        // The progress bar, only while btaudio is reporting a position. An empty
        // outline the rest of the time would read as a track at zero.
        const int by = ui::TOP + 3 * ui::ROWH - 1;
        if (g_media_prog >= 0) {
            c.rect(0, by, c.width(), 4, 1);
            const int fill = (c.width() - 2) * g_media_prog / 100;
            if (fill > 0) c.fill_rect(1, by + 1, fill, 2, 1);
        } else {
            c.hline(0, by + 1, c.width(), 1);
        }

        // The transport. A busy worker takes the strip's place, because a
        // command is out and pressing another one is refused anyway.
        const int sy = by + 6;
        if (media_job_running()) {
            c.text_centred(sy + 3, busy_word(), 1);
            c.spinner(c.width() / 2 - 2, sy + 12, phase_ / MEDIA_SPIN_MS, 1);
            return;
        }
        const int cw = c.width() / MEDIA_C_COUNT;
        for (int i = 0; i < MEDIA_C_COUNT; i++) {
            const int x = i * cw;
            const bool on = (i == sel_);
            // Outlined is "the cursor is here"; filled is "this control has the
            // knob", which only the volume one ever does.
            if (on) c.rounded_rect(x + 1, sy, cw - 2, 15, 1, vol_mode_);
            const int col = (on && vol_mode_) ? 0 : 1;
            media_glyph(c, x + cw / 2 - media_glyph_w(i) / 2, sy + 7, i, col);
        }
    }

    Action on_event(Event e) override {
        if (vol_mode_) {
            if (e == EV_ROT_CW)  { nudge_volume(+5); return ui::ACT_STAY; }
            if (e == EV_ROT_CCW) { nudge_volume(-5); return ui::ACT_STAY; }
            // Both ways out of the mode, because BACK meaning "cancel this
            // mode" and BACK meaning "leave the screen" cannot both be true and
            // the mode is the nearer of the two.
            if (e == EV_SELECT || e == EV_BACK) {
                vol_mode_ = false;
                flush_volume();
                return ui::ACT_STAY;
            }
            if (e == EV_HOME) { vol_mode_ = false; flush_volume(); }
            return Screen::on_event(e);
        }

        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % MEDIA_C_COUNT; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + MEDIA_C_COUNT - 1) % MEDIA_C_COUNT; return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) { stop_now(); return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (media_job_running()) return ui::ACT_STAY;
            switch (sel_) {
                case MEDIA_C_PREV: step(-1); break;
                case MEDIA_C_NEXT: step(+1); break;
                case MEDIA_C_VOL:  vol_mode_ = true; break;
                default:
                    if (g_media_state == MEDIA_PLAYING) stop_now();
                    else                                start_now();
                    break;
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_;
    bool     vol_mode_;
    uint32_t vol_send_;         // ms left before the pending volume is sent
    uint32_t phase_, poll_, grace_;
    int      meta_pos_;         // the queue position meta_ describes
    MediaWav meta_;
    char     stem_[26];

    // Playing beats every other reading, because it can only be true when
    // something is connected — and it is known one reply sooner than the link
    // is, straight after btaudio answers a play.
    const char *state_word(void) const {
        if (!g_media_known)                 return "--";
        if (g_media_state == MEDIA_PLAYING) return "playing";
        if (!g_media_linked)                return "no speaker";
        return "stopped";
    }

    const char *busy_word(void) const {
        switch (g_media_want) {
            case MEDIA_W_PLAY: return "Starting";
            case MEDIA_W_STOP: return "Stopping";
            default:           return "Working";
        }
    }

    // Read the header of whatever is under the cursor. Once per track, not once
    // per frame: it is a supervisor call and a flash read.
    void refresh_meta(void) {
        if (meta_pos_ == g_media_pos) return;
        meta_pos_ = g_media_pos;
        memset(&meta_, 0, sizeof(meta_));
        stem_[0] = 0;
        char name[FW_NAME_MAX];
        if (!media_track_name(g_media_pos, name, sizeof(name))) return;
        media_stem(stem_, sizeof(stem_), name);
        char path[MEDIA_PATH_MAX];
        if (media_track_path(g_media_pos, path, sizeof(path)))
            media_wav_read(path, &meta_);
    }

    void start_now(void) {
        if (g_media_pos < 0 && g_media_qn) g_media_pos = 0;
        media_play_at(g_media_pos);         // its reason is reported from tick
    }

    void stop_now(void) {
        // The grace window stops the status poll reading this as a track that
        // reached its end and helpfully starting the next one.
        grace_ = 4000;
        media_send(MEDIA_W_STOP, "btaudio stop");
    }

    void step(int by) {
        if (!g_media_qn) return;
        int at = g_media_pos < 0 ? 0 : g_media_pos + by;
        if (at < 0) at = g_media_qn - 1;
        if (at >= g_media_qn) at = 0;
        const bool was_playing = (g_media_state == MEDIA_PLAYING);
        g_media_pos = at;
        refresh_meta();
        // Skipping while stopped moves the cursor and leaves it stopped, which
        // is what somebody choosing a track before starting it wants.
        if (was_playing) {
            grace_ = 4000;
            // btaudio refuses a play while one is streaming, so the stop has to
            // land first. The reply from it starts the new track — see below.
            resume_after_stop_ = media_send(MEDIA_W_STOP, "btaudio stop");
        }
    }

    void track_ended(void) {
        // btaudio's feeder pauses the stream at the end of the file, so a poll
        // that finds nothing playing when something was is a track that finished
        // rather than one somebody stopped.
        if (g_media_pos + 1 >= g_media_qn) return;   // the folder is done
        g_media_pos++;
        refresh_meta();
        grace_ = 4000;
        media_play_at(g_media_pos);
    }

    void nudge_volume(int by) {
        // Nothing has said what the volume is yet, so start from btaudio's own
        // default rather than from zero — and the send below makes it true.
        int v = g_media_vol < 0 ? 80 : g_media_vol;
        v += by;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_media_vol = v;
        vol_send_ = 400;
    }

    void flush_volume(void) {
        if (!vol_send_ || g_media_vol < 0) { vol_send_ = 0; return; }
        vol_send_ = 0;
        // Volume touches no radio and no file — it scales samples on their way
        // to the encoder — so it goes inline rather than through the worker, the
        // same as the status poll and for the same reason.
        // A change that cannot be sent because the radio is busy is DROPPED
        // rather than queued. The next detent sends the whole value again — it
        // is an absolute level, not a step — so nothing is lost that turning the
        // knob once more does not fix.
        if (!media_job_idle()) return;
        char line[32];
        snprintf(line, sizeof(line), "btaudio volume %d", g_media_vol);
        // Captured and thrown away rather than run with no capture at all: an
        // uncaptured command prints to the console, and a knob being turned
        // would put a line of "[@] Volume 65%" on somebody's serial session for
        // every detent.
        fw_shell_run(line, g_media_out, sizeof(g_media_out));
    }

    void finished(void) {
        if (!g_media_expect) return;
        g_media_expect = false;
        media_first_line(g_media_out, g_media_note, sizeof(g_media_note));

        if (g_media_want == MEDIA_W_STOP) {
            g_media_state = MEDIA_IDLE;
            g_media_prog  = -1;
            g_media_known = true;
            if (resume_after_stop_) {
                resume_after_stop_ = false;
                media_play_at(g_media_pos);
                return;
            }
        } else if (g_media_want == MEDIA_W_PLAY) {
            // btaudio answers "Playing <file> (...)" or an error, and the error
            // is the only place "Nothing to play to" is ever said.
            const bool started = strstr(g_media_out, "Playing ") != nullptr;
            g_media_state = started ? MEDIA_PLAYING : MEDIA_IDLE;
            g_media_known = true;
            // The feeder needs a moment before status reports a stream, so the
            // next poll must not read the gap as a track that already finished.
            if (started) grace_ = 4000;
            if (!started && g_media_note[0]) ui::notice("Media", g_media_note);
        }
        g_media_want = MEDIA_W_NONE;
        refresh_meta();
        poll_ = MEDIA_POLL_MS;          // read the truth back on the next tick
    }

    bool resume_after_stop_;
};

static void media_open_now(void) {
    MediaNowScreen *s = gui::push<MediaNowScreen>();
    if (s) s->begin();
}

// --- the browser -------------------------------------------------------------------
//
// Folders and .wav files, and nothing else. A music browser that lists the
// config files beside the tracks is a file manager, and there is already one of
// those two rows up the Tools folder.

// The rows of one level: the directory index and whether it is a folder. Held
// here rather than in the screen for the reason at the top of the file — a slot
// is 384 bytes — and rebuilt on entry to a level rather than kept per level,
// because only the top screen is ever drawn.
#define MEDIA_ROW_MAX 96

static char     g_media_path[MEDIA_LEVEL_MAX][MEDIA_PATH_MAX];
static uint16_t g_media_row[MEDIA_ROW_MAX];
static uint8_t  g_media_dir_row[MEDIA_ROW_MAX];
static int      g_media_rows;

class MediaBrowseScreen : public Screen {
public:
    void begin(int level) {
        level_ = level;
        sel_ = top_ = 0;
        ready_ = true;
        detail_for_ = -1;
        relist();
    }

    // The folder IS the title, the same as the file browser: there is no room
    // for a path row and the status bar is already there. It is set from the
    // path rather than computed in draw(), because the runner paints the bar
    // BEFORE draw() runs and a title decided there arrives a frame late.
    const char *title(void) const override {
        return ready_ ? media_base(g_media_path[level_]) : "Music";
    }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT opens a folder, or";
        out[1] = "plays a track from here on.";
        out[2] = "Hold SELECT on a folder to";
        out[3] = "play the whole of it.";
        return 4;
    }

    // Called on the way in, and again every time a child pops — which is what
    // makes the rows agree with the folder after coming back up a level. The
    // guard is for the very first call, which push_commit() makes BEFORE
    // begin() has run and therefore on a slot still holding the last screen's
    // bytes: level_ is checked as well as ready_, because a stale bool reads as
    // true and a stale int would index the path stack out of it.
    void enter(void) override {
        if (!ready_ || level_ < 0 || level_ >= MEDIA_LEVEL_MAX) return;
        relist();
    }

    void draw(Canvas &c) override {
        if (!g_media_rows) {
            c.text(2, ui::TOP + ui::ROWH, "No music here.", 1);
            c.text(2, ui::TOP + 2 * ui::ROWH, ".wav files only —", 1);
            c.text(2, ui::TOP + 3 * ui::ROWH, "that is what plays.", 1);
            return;
        }

        // One row of the panel is the detail line for whatever is under the
        // cursor, which is where a track's format and length go. Five rows and a
        // real answer beats six rows and a list of bare names.
        const int rows = ui::rows_for(c) - 1;
        if (sel_ >= g_media_rows) sel_ = g_media_rows - 1;
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        if (top_ < 0) top_ = 0;

        const bool scrolls = g_media_rows > rows;
        const int right = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        FwDirEntry e;
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= g_media_rows) break;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            const int col = on ? 0 : 1;
            if (fw_dir_entry(g_media_path[level_], g_media_row[idx], &e) != 1) continue;
            if (g_media_dir_row[idx]) {
                // A trailing '/' marks a folder, which costs one character where
                // a column of icons would cost six pixels on every row — and it
                // survives the inversion of the selected row.
                char label[FW_NAME_MAX + 2];
                snprintf(label, sizeof(label), "%s/", e.name);
                c.text_fit(3, y, label, col, right - 6, false);
            } else {
                char stem[FW_NAME_MAX];
                media_stem(stem, sizeof(stem), e.name);
                // A speck against the track that is the current one, so a folder
                // somebody came back to says where they were.
                const bool here = playing_row(idx);
                if (here) c.fill_rect(right - 5, y + 2, 3, 3, col);
                c.text_fit(3, y, stem, col, right - (here ? 10 : 6), false);
            }
        }

        if (scrolls)
            c.scrollbar(right + 1, ui::TOP, c.height() - ui::TOP - ui::ROWH,
                        top_, rows, g_media_rows);

        const int dy = c.height() - ui::ROWH;
        c.hline(0, dy - 1, c.width(), 1);
        detail(sel_);
        c.text_fit(2, dy, detail_, 1, c.width() - 4, true);
    }

    Action on_event(Event e) override {
        if (!g_media_rows) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % g_media_rows; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + g_media_rows - 1) % g_media_rows; return ui::ACT_STAY; }
        if (e == EV_SELECT)      { open_sel(false); return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) { open_sel(true);  return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int  level_, sel_, top_;
    bool ready_;
    int  detail_for_;
    char detail_[40];

    void relist(void) {
        g_media_rows = 0;
        const char *dir = g_media_path[level_];
        const int n = fw_dir_count(dir);
        FwDirEntry e;
        for (int i = 0; i < n && g_media_rows < MEDIA_ROW_MAX; i++) {
            if (fw_dir_entry(dir, (unsigned)i, &e) != 1) break;
            if (!e.is_dir && !media_is_wav(e.name)) continue;
            g_media_row[g_media_rows] = (uint16_t)i;
            g_media_dir_row[g_media_rows] = e.is_dir ? 1 : 0;
            g_media_rows++;
        }
        // Clamp rather than reset. Coming back from a subfolder should land on
        // the row it was entered from.
        if (sel_ >= g_media_rows) sel_ = g_media_rows ? g_media_rows - 1 : 0;
        if (sel_ < 0) sel_ = 0;
        detail_for_ = -1;
    }

    // Is this row the track the player is on? Only meaningful while the queue
    // came from the folder being looked at.
    bool playing_row(int idx) const {
        if (g_media_pos < 0 || g_media_pos >= g_media_qn) return false;
        if (strcmp(g_media_qdir, g_media_path[level_]) != 0) return false;
        return g_media_q[g_media_pos] == g_media_row[idx];
    }

    // The line under the rule: what the cursor is on, in as much detail as one
    // row holds. Cached, because it costs a header read.
    void detail(int idx) {
        if (idx == detail_for_) return;
        detail_for_ = idx;
        detail_[0] = 0;
        FwDirEntry e;
        if (fw_dir_entry(g_media_path[level_], g_media_row[idx], &e) != 1) return;
        if (g_media_dir_row[idx]) {
            nova::copy(detail_, sizeof(detail_), "folder - hold to play it all");
            return;
        }
        char path[MEDIA_PATH_MAX];
        if (!media_join(path, sizeof(path), g_media_path[level_], e.name)) {
            nova::copy(detail_, sizeof(detail_), "the path is too long to play");
            return;
        }
        MediaWav w;
        media_wav_read(path, &w);
        if (w.why == MEDIA_WAV_OK && w.artist[0]) {
            char dur[10];
            media_mmss(dur, sizeof(dur), media_seconds(w));
            snprintf(detail_, sizeof(detail_), "%s  %s", w.artist, dur);
        } else {
            media_format_line(detail_, sizeof(detail_), w);
        }
    }

    void open_sel(bool held) {
        FwDirEntry e;
        if (fw_dir_entry(g_media_path[level_], g_media_row[sel_], &e) != 1) return;

        if (g_media_dir_row[sel_]) {
            char path[MEDIA_PATH_MAX];
            if (!media_join(path, sizeof(path), g_media_path[level_], e.name)) {
                ui::notice("Music", "That path is longer than this screen can hold.");
                return;
            }
            if (held) { play_folder(path); return; }
            if (level_ + 1 >= MEDIA_LEVEL_MAX || gui::depth() + 1 >= gui::STACK_MAX) {
                ui::notice("Music", "As deep as this browser goes. Set the music "
                                    "folder in Settings to start further in.");
                return;
            }
            nova::copy(g_media_path[level_ + 1], MEDIA_PATH_MAX, path);
            MediaBrowseScreen *s = gui::push<MediaBrowseScreen>();
            if (s) s->begin(level_ + 1);
            return;
        }

        // A track. The QUEUE is the folder it is in, and playing starts here
        // rather than at the top of it — choosing the fourth song and being
        // given the first is not what the press meant.
        media_queue_build(g_media_path[level_]);
        int at = 0;
        for (int i = 0; i < g_media_qn; i++)
            if (g_media_q[i] == g_media_row[sel_]) { at = i; break; }
        g_media_pos = at;
        media_open_now();
        media_play_at(at);
    }

    void play_folder(const char *path) {
        if (!media_queue_build(path)) {
            ui::notice("Music", "No .wav files in that folder.");
            return;
        }
        g_media_pos = 0;
        media_open_now();
        media_play_at(0);
    }
};

// --- the speaker -------------------------------------------------------------------
//
// One address, remembered, and a way to find a new one. `btaudio` has no
// discovery of its own, so the list comes from `bt scan classic` — the inquiry
// that finds a speaker in pairing mode, where an LE scan would not: A2DP is
// Classic Bluetooth and a speaker's LE advertisement, if it has one at all, is a
// different address from the one that accepts audio.

#define MEDIA_SCAN_MAX 12

struct MediaFound {
    char mac[18];
    char name[18];
};
static MediaFound g_media_found[MEDIA_SCAN_MAX];
static int        g_media_found_n;

static bool media_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Seventeen characters of "XX:XX:XX:XX:XX:XX" and nothing else. A line is a
// device only if it carries a well-formed address, which is what keeps the
// command's own tagged lines and its footnotes out of the list.
static bool media_looks_mac(const char *p) {
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) { if (p[i] != ':') return false; }
        else if (!media_hex(p[i])) return false;
    }
    return true;
}

// Rows as os/shell/bt.cpp prints them:
//
//     "  C8:7B:23:11:04:9A  BR    -62 dBm  JBL Flip 5"
//
// The name is whatever follows the reading, and it may be empty. Everything
// before it is fixed-width, but the address is found rather than assumed so a
// row that gains a column does not silently read the wrong field.
static int media_scan_parse(const char *text) {
    g_media_found_n = 0;
    for (const char *p = text; *p && g_media_found_n < MEDIA_SCAN_MAX; ) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p) p++;

        while (line < end && *line == ' ') line++;
        if (end - line < 17 || !media_looks_mac(line)) continue;

        MediaFound &f = g_media_found[g_media_found_n];
        for (int i = 0; i < 17; i++) f.mac[i] = line[i];
        f.mac[17] = 0;
        f.name[0] = 0;

        // Past the address, the kind, and the reading if there was one. The name
        // is the last field, and "dBm" is the marker that the reading is over.
        const char *q = line + 17;
        const char *dbm = nullptr;
        for (const char *s = q; s + 3 <= end; s++)
            if (s[0] == 'd' && s[1] == 'B' && s[2] == 'm') { dbm = s + 3; break; }
        if (dbm) q = dbm;
        while (q < end && *q == ' ') q++;
        unsigned n = 0;
        while (q < end && n + 1 < sizeof(f.name)) f.name[n++] = *q++;
        while (n && f.name[n - 1] == ' ') n--;
        f.name[n] = 0;
        g_media_found_n++;
    }
    return g_media_found_n;
}

class MediaSpeakerScreen : public Screen {
public:
    void begin(void) {
        sel_ = 0;
        top_ = 0;
        phase_ = 0;
        scanning_ = false;
        saved_[0] = 0;
        pending_[0] = 0;
    }

    const char *title(void) const override { return "Speaker"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT connects to the row.";
        out[1] = "A speaker has to be in";
        out[2] = "pairing mode to be found.";
        out[3] = "Hold SELECT to forget.";
        return 4;
    }

    void enter(void) override { media_job_disown(); }
    void leave(void) override { media_job_disown(); }

    bool animating(void) const override { return media_job_running(); }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        if (media_job_take()) { finished(); return true; }
        // The same reporter Now Playing uses, so a refused scan says the same
        // thing a refused play does rather than growing a second wording.
        if (media_report_fail()) return true;
        return media_job_running();
    }

    void draw(Canvas &c) override {
        if (media_job_running()) {
            c.text_centred(ui::TOP + ui::ROWH, scanning_ ? "Scanning" : "Connecting", 1);
            c.text_centred(ui::TOP + 2 * ui::ROWH,
                           scanning_ ? "about ten seconds" : "up to ten seconds", 1);
            c.spinner(c.width() / 2 - 2, ui::TOP + 4 * ui::ROWH, phase_ / MEDIA_SPIN_MS, 1);
            return;
        }

        const int rows = ui::rows_for(c);
        const int n = row_count();
        if (sel_ >= n) sel_ = n - 1;
        if (sel_ < 0) sel_ = 0;
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        const bool scrolls = n > rows;
        const int right = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        char label[40];
        for (int i = 0; i < rows && top_ + i < n; i++) {
            const int idx = top_ + i;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            const int col = on ? 0 : 1;
            row_label(idx, label, sizeof(label));
            c.text_fit(3, y, label, col, right - 6, true);
        }
        if (scrolls)
            c.scrollbar(right + 1, ui::TOP, c.height() - ui::TOP, top_, rows, n);
    }

    Action on_event(Event e) override {
        if (media_job_running()) return Screen::on_event(e);
        const int n = row_count();
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) {
            // Forgetting only means anything on the saved-speaker row. It used
            // to fall through in silence on the other two kinds, which is two
            // rows out of three where the help's promise did nothing.
            if (sel_ != 0) {
                ui::notice("Speaker", "Hold SELECT on the top row to forget the "
                                      "speaker this device remembers.");
                return ui::ACT_STAY;
            }
            if (!saved_[0]) {
                ui::notice("Speaker", "No speaker is saved, so there is nothing "
                                      "to forget.");
                return ui::ACT_STAY;
            }
            // Forgetting says so by itself: the top row is the saved speaker,
            // and it becomes the absence of one on the next frame.
            nova::reg_set(MEDIA_KEY_SPEAKER, "");
            nova::reg_save();
            saved_[0] = 0;
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT) { activate(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    uint32_t phase_;
    bool     scanning_;
    char     saved_[20];

    // Row 0 is the saved speaker (or the absence of one), row 1 is the scan, and
    // whatever a scan found comes after.
    int row_count(void) const { return 2 + g_media_found_n; }

    void row_label(int idx, char *out, unsigned cap) {
        if (idx == 0) {
            nova::copy(saved_, sizeof(saved_), nova::reg(MEDIA_KEY_SPEAKER, ""));
            if (!saved_[0]) { nova::copy(out, cap, "No speaker saved"); return; }
            snprintf(out, cap, "%s %s", g_media_linked ? "*" : " ", saved_);
            return;
        }
        if (idx == 1) { nova::copy(out, cap, "Scan for speakers"); return; }
        const MediaFound &f = g_media_found[idx - 2];
        if (f.name[0]) snprintf(out, cap, "%s", f.name);
        else           snprintf(out, cap, "%s", f.mac);
    }

    void activate(void) {
        if (sel_ == 1) {
            // Ten seconds of inquiry. Long enough to find a speaker that was
            // switched to pairing a moment ago, short enough that somebody does
            // not conclude the device has stopped.
            scanning_ = true;
            g_media_found_n = 0;
            media_send(MEDIA_W_SCAN, "bt scan classic 10");
            return;
        }
        char mac[20];
        if (sel_ == 0) {
            nova::copy(mac, sizeof(mac), nova::reg(MEDIA_KEY_SPEAKER, ""));
            if (!mac[0]) {
                ui::notice("Speaker", "Nothing saved yet. Scan, and connecting "
                                      "to one remembers it.");
                return;
            }
        } else {
            nova::copy(mac, sizeof(mac), g_media_found[sel_ - 2].mac);
        }
        nova::copy(pending_, sizeof(pending_), mac);
        scanning_ = false;
        char line[48];
        snprintf(line, sizeof(line), "btaudio connect %s", mac);
        media_send(MEDIA_W_CONNECT, line);
    }

    void finished(void) {
        if (!g_media_expect) return;
        g_media_expect = false;
        const uint8_t what = g_media_want;
        g_media_want = MEDIA_W_NONE;

        if (what == MEDIA_W_SCAN) {
            scanning_ = false;
            if (!g_media_out[0]) {
                ui::notice("Speaker", "The scan printed nowhere — something else "
                                      "had the output. Try it again.");
                return;
            }
            if (!media_scan_parse(g_media_out))
                ui::notice("Speaker", "Nothing answered. A speaker only shows up "
                                      "while it is in pairing mode.");
            sel_ = g_media_found_n ? 2 : 1;
            top_ = 0;
            return;
        }

        // btaudio prints "Connected to <address>." and nothing else says that,
        // so it is the one line worth matching on. Everything else — refused,
        // no answer, no Bluetooth on this board — is shown in its own words.
        const bool linked = strstr(g_media_out, "Connected to") != nullptr;
        g_media_linked = linked;
        g_media_known  = true;
        if (linked) {
            nova::copy(g_media_peer, sizeof(g_media_peer), pending_);
            // Remembered only on success. Saving an address that did not answer
            // means the next boot offers a speaker that was never there.
            nova::reg_set(MEDIA_KEY_SPEAKER, pending_);
            nova::reg_save();
        }
        media_first_line(g_media_out, g_media_note, sizeof(g_media_note));
        ui::notice("Speaker", g_media_note[0] ? g_media_note
                                              : "No answer came back from btaudio.");
    }

    char pending_[20];
};

// --- the front page ---------------------------------------------------------------
//
// What the catalogue opens. Two rows of state and three ways in — and it is the
// screen whose title has to be the catalogue's label, so it is called Media and
// the ones below it are called what they are.

class MediaScreen : public Screen {
public:
    void begin(void) { sel_ = 0; poll_ = MEDIA_POLL_MS; }

    const char *title(void) const override { return "Media"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "Music browses for a track.";
        out[1] = "Speaker finds one to play to.";
        out[2] = "A folder is the queue.";
        out[3] = "16-bit WAV only.";
        return 4;
    }

    void enter(void) override {
        media_job_disown();
        poll_ = MEDIA_POLL_MS;
    }

    bool tick(uint32_t dt) override {
        poll_ += dt;
        if (poll_ < MEDIA_POLL_MS || media_job_running()) return false;
        poll_ = 0;
        media_poll_now();
        return true;
    }

    void draw(Canvas &c) override {
        char line[40];

        // The speaker, by the truest thing known about it: connected beats
        // saved, and saved beats nothing.
        c.text(0, ui::TOP, "Spk", 1);
        const char *saved = nova::reg(MEDIA_KEY_SPEAKER, "");
        if (g_media_linked && g_media_peer[0]) snprintf(line, sizeof(line), "%s", g_media_peer);
        else if (saved[0])                     snprintf(line, sizeof(line), "%s (saved)", saved);
        else                                   snprintf(line, sizeof(line), "none set");
        c.text_fit(24, ui::TOP, line, 1, c.width() - 24, true);

        const int y2 = ui::TOP + ui::ROWH;
        c.text(0, y2, "Now", 1);
        if (g_media_state == MEDIA_PLAYING && g_media_now[0])
            snprintf(line, sizeof(line), "%s", media_base(g_media_now));
        else if (!g_media_known) snprintf(line, sizeof(line), "--");
        else                     snprintf(line, sizeof(line), "nothing playing");
        c.text_fit(24, y2, line, 1, c.width() - 24, true);

        const int ry = y2 + ui::ROWH - 1;
        c.hline(0, ry, c.width(), 1);

        static const char *kRows[] = { "Music", "Now Playing", "Speaker" };
        for (int i = 0; i < 3; i++) {
            const int y = ry + 3 + i * ui::ROWH;
            const bool on = (i == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
            c.text(3, y, kRows[i], on ? 0 : 1);
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % 3; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + 2) % 3; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (sel_ == 0) open_browser();
            else if (sel_ == 1) media_open_now();
            else {
                MediaSpeakerScreen *s = gui::push<MediaSpeakerScreen>();
                if (s) s->begin();
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_;
    uint32_t poll_;

    // The music folder, from the registry, falling back to the root when it is
    // not there. A device with no /nova/music yet gets a browser it can find its
    // way out of rather than an empty screen with no explanation.
    void open_browser(void) {
        const char *root = nova::reg(MEDIA_KEY_ROOT, MEDIA_ROOT_DEF);
        if (!root[0]) root = MEDIA_ROOT_DEF;
        nova::copy(g_media_path[0], MEDIA_PATH_MAX, root);
        if (fw_dir_count(g_media_path[0]) < 0)
            nova::copy(g_media_path[0], MEDIA_PATH_MAX, "/");
        MediaBrowseScreen *s = gui::push<MediaBrowseScreen>();
        if (s) s->begin(0);
    }
};

void open_media(void) {
    MediaScreen *s = gui::push<MediaScreen>();
    if (s) s->begin();
}

}  // namespace screens
}  // namespace nova
