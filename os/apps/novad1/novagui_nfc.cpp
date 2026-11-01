// Desc: The NFC app - read a card, save its identity, dump its memory, and the library.
// File: novagui_nfc.cpp
//
// v1's NFC app read a UID and showed it; its Save wrote a Flipper .nfc, and a full memory dump was
// "the next increment". This is that increment. Read a card, save it at the UID/ATQA/SAK level,
// dump its memory (Mifare Classic with the well-known keys, or an NTAG/Ultralight whole) into a
// .nfc, and browse what has been saved - the same four things the Sub-GHz app has, in the same
// shape, because a person who has used one should not have to learn the other.
//
// The read and the dump run on a WORKER TASK. A scan is seconds and a 4K dump is longer still, and
// either on the UI loop is a panel that has stopped - which from the outside is a panel that has
// crashed. So the screen asks, the worker answers, and the screen polls a flag, exactly as
// Wardrive and Sub-GHz do.
//
// DEVICE-UNCONFIRMED: the framing and the walk are host-tested (nfcframe_test, novacodec_test); no
// PN532 has been on the end of the wire.
#include "novagui_nfc.h"
#include "novagui.h"
#include "novagui_codes.h"
#include "novakeys.h"
#include "novacore.h"
#include "novanfc.h"
#include "novanfcdump.h"
#include "novaboard.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;
using nfcdump::Tag;

// --- the reader, off the UI thread ----------------------------------------------------
//
// Everything the worker touches is a file static: BACK is one press and a dump is many seconds, so
// the screen that asked is routinely gone before the answer is ready, and a worker writing into a
// pool slot that has since been handed to another screen is the bug this avoids.

static volatile bool g_nfc_run;         // the worker should keep going
static volatile bool g_nfc_live;        // and it is still there
static volatile int  g_nfc_mode;        // what it is doing: NFC_READ or NFC_DUMP
static volatile int  g_nfc_done, g_nfc_total;
static int           g_nfc_sda, g_nfc_scl;
static bool          g_nfc_have;        // a card has been read since the screen opened
static Tag           g_nfc_tag;
static char          g_nfc_kind[20];    // "MIFARE Classic 1K", ...
static char          g_nfc_msg[28];
static char          g_nfc_saved[CODES_NAME_MAX];   // what a save wrote, for the message
static bool          g_nfc_save_after;  // a dump that should write a file when it finishes

enum { NFC_READ, NFC_DUMP };

// The dump buffer is big (a 4K card is 4 KB of memory plus its masks) and used only while a dump is
// running, so it is taken from the heap for the length of one dump rather than carried as resident
// bss that every Nova D1 pays for whether or not it reads a card.

#define NFC_READ_MS 5000

// The firmware's two verbs, wired to the ABI. The pins are the ones the screen captured when it
// started the worker, held in statics so a Reader with a null ctx can reach them.
static int dev_select(void *, Tag *out, bool reset) {
    FwNfcTag t;
    const int r = fw_nfc_select((unsigned)g_nfc_sda, (unsigned)g_nfc_scl, &t, NFC_READ_MS,
                                reset ? FW_NFC_RESET_FIELD : 0u);
    if (r == 1) {
        memset(out, 0, sizeof(*out));
        memcpy(out->uid, t.uid, t.uid_len);
        out->uid_len = t.uid_len;
        out->atqa[0] = t.atqa[0]; out->atqa[1] = t.atqa[1];
        out->sak = t.sak;
    }
    return r;
}
static int dev_exchange(void *, const uint8_t *tx, unsigned tx_len, uint8_t *rx, unsigned rx_cap) {
    return fw_nfc_exchange((unsigned)g_nfc_sda, (unsigned)g_nfc_scl, tx, tx_len, rx, rx_cap);
}
static bool dev_cancelled(void *) { return !g_nfc_run || fw_task_should_stop() != 0; }
static void dev_progress(void *, unsigned done, unsigned total) {
    g_nfc_done = (int)done; g_nfc_total = (int)total; gui::invalidate();
}

static nfcdump::Reader dev_reader(void) {
    nfcdump::Reader r;
    r.select = dev_select; r.exchange = dev_exchange;
    r.cancelled = dev_cancelled; r.progress = dev_progress; r.ctx = nullptr;
    return r;
}

// The card's name for the UI, and the file's default name from its UID.
static void set_kind(const Tag &t) {
    nova::copy(g_nfc_kind, sizeof(g_nfc_kind), nfc::identify(t.sak, t.atqa, 2, 0).device_type);
    const nfc::Kind k = nfc::identify(t.sak, t.atqa, 2, 0);
    if (k.sub_type) {
        char full[20];
        snprintf(full, sizeof(full), "%s %s", k.device_type, k.sub_type);
        nova::copy(g_nfc_kind, sizeof(g_nfc_kind), full);
    }
}
static void uid_name(const Tag &t, char *out, unsigned cap) {
    char hex[24];
    unsigned at = 0;
    for (unsigned i = 0; i < t.uid_len && at + 2 < sizeof(hex); i++)
        at += (unsigned)snprintf(hex + at, sizeof(hex) - at, "%02x", t.uid[i]);
    snprintf(out, cap, "card_%s", hex);
}

// Open a uniquely-named file in the library for this card's UID. Returns the sink; `saved` gets the
// name written to it.
static codec::Sink open_card_file(const Tag &t, char *saved, unsigned cap) {
    char base[CODES_NAME_MAX], clean[CODES_NAME_MAX];
    uid_name(t, base, sizeof(base));
    if (!codes::clean_name(base, clean, sizeof(clean))) nova::copy(clean, sizeof(clean), "card");
    codes::unique_name(codes::kNfc, clean, saved, cap);
    codes::ensure_dir(codes::kNfc);
    char path[96];
    codes::path_for(codes::kNfc, saved, path, sizeof(path));
    return codes::file_sink_open(path);
}

// A finished memory dump.
static bool save_dump(const nfcdump::Dump &d, char *saved, unsigned cap) {
    codec::Sink k = open_card_file(d.tag, saved, cap);
    bool ok;
    if (d.type == nfcdump::DUMP_CLASSIC) {
        ok = nfc::write_classic(k, d.tag.uid, d.tag.uid_len, d.tag.atqa, d.tag.sak, d.kind,
                                d.units, nfcdump::unit_getter, (void *)&d);
    } else if (d.type == nfcdump::DUMP_ULTRALIGHT) {
        nfc::UltralightExtras x = {};
        if (d.has_sig) x.signature = d.sig;
        if (d.has_ver) x.mifare_version = d.mifare_version;
        x.pages_read = (int)d.read_units;
        ok = nfc::write_ultralight(k, d.tag.uid, d.tag.uid_len, d.tag.atqa, d.tag.sak, d.kind,
                                   d.units, nfcdump::unit_getter, (void *)&d, &x);
    } else {
        ok = nfc::write_iso14443a(k, d.tag.uid, d.tag.uid_len, d.tag.atqa, d.tag.sak);
    }
    return codes::file_sink_done() && ok;
}

static int nfc_worker(void *) {
    g_nfc_live = true;
    g_nfc_done = g_nfc_total = 0;

    Tag first;
    const int sel = dev_select(nullptr, &first, /*reset*/true);
    if (sel == 1) {
        g_nfc_tag = first;
        g_nfc_have = true;
        set_kind(first);
        if (g_nfc_mode == NFC_READ) {
            nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "card read");
        } else {
            nfcdump::Dump *d = (nfcdump::Dump *)fw_malloc(sizeof(nfcdump::Dump));
            if (!d) {
                nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "heap too full");
            } else {
                nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "dumping...");
                gui::invalidate();
                const int rc = nfcdump::run(dev_reader(), first, d);
                if (rc == nfcdump::DUMP_CANCELLED) {
                    nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "stopped");
                } else if (g_nfc_save_after) {
                    if (save_dump(*d, g_nfc_saved, sizeof(g_nfc_saved)))
                        snprintf(g_nfc_msg, sizeof(g_nfc_msg), "Saved %.18s", g_nfc_saved);
                    else
                        nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "save failed");
                }
                fw_free(d);
            }
        }
    } else if (sel == 0) {
        nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "No card");
    } else if (sel == FW_NFC_BADPINS) {
        nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "check the pins");
    } else {
        nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "no reader");
    }

    g_nfc_run = false;
    g_nfc_live = false;
    gui::invalidate();
    return 0;
}

// --- the screen -----------------------------------------------------------------------

class NfcScreen : public Screen {
public:
    const char *title(void) const override { return "NFC"; }

    int help(const char **out, int max) const override {
        if (max < 5) return 0;
        out[0] = "Hold a card flat on the";
        out[1] = "antenna. Read shows what it";
        out[2] = "is; Dump saves its memory";
        out[3] = "to /nova/codes/nfc.";
        out[4] = "Needs d1 pins set sda/scl.";
        return 5;
    }

    void enter(void) override {
        phase_ = 0;
        if (!started_) {
            started_ = true;
            sel_ = 0;
            g_nfc_have = false;
            g_nfc_msg[0] = 0;
            wired_ = board::pin(board::PIN_SDA) != board::PIN_NONE &&
                     board::pin(board::PIN_SCL) != board::PIN_NONE;
        }
    }

    bool animating(void) const override { return g_nfc_live; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        return g_nfc_live;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        char line[40];

        if (!wired_) {
            c.text(2, y, "No pins set.", 1); y += ui::ROWH;
            ui::text_block(c, 2, y, "Set them with d1 pins set sda, then scl.", 3);
            return;
        }

        // The card, if one has been read; else what to do.
        if (g_nfc_have) {
            char uid[32];
            unsigned at = 0;
            for (unsigned i = 0; i < g_nfc_tag.uid_len && at + 3 < sizeof(uid); i++)
                at += (unsigned)snprintf(uid + at, sizeof(uid) - at, "%s%02X", i ? " " : "", g_nfc_tag.uid[i]);
            c.text_scroll(2, y, uid, 1, c.width() - 12, false);
            y += ui::ROWH;
            c.text_fit(2, y, g_nfc_kind, 1, c.width() - 4, false);
            y += ui::ROWH;
        } else {
            c.text(2, y, g_nfc_live ? "working..." : "No card yet", 1);
            y += ui::ROWH;
        }

        // A dump's progress, or the last message.
        if (g_nfc_live && g_nfc_mode == NFC_DUMP && g_nfc_total) {
            snprintf(line, sizeof(line), "%d / %d", g_nfc_done, g_nfc_total);
            c.text(2, y, line, 1);
        } else if (g_nfc_msg[0]) {
            c.text_scroll(2, y, g_nfc_msg, 1, c.width() - 12, false);
        }
        if (g_nfc_live) c.spinner(c.width() - 9, y, phase_ / 120, 1);
        y += ui::ROWH;

        // The actions.
        const int rows = (c.height() - y) / ui::ROWH;
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        for (int i = top_; i < ACT_N && i < top_ + rows; i++, y += ui::ROWH) {
            char label[16];
            action_label(i, label, sizeof(label));
            const bool on = i == sel_;
            if (on) c.fill_rect(0, y - 1, c.width() - ui::SB_W - 1, ui::ROWH, 1);
            c.text_fit(2, y, label, on ? 0 : 1, c.width() - ui::SB_W - 6, false);
        }
    }

    Action on_event(Event e) override {
        if (!wired_) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % ACT_N; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + ACT_N - 1) % ACT_N; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) { run_action(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

    void leave(void) override {
        // A worker that is still going when this screen is left for good must be told to stop; a
        // child pushed over it (the library, a notice) must not.
        if (gui::top() == this) return;
        g_nfc_run = false;
    }

private:
    enum { ACT_READ, ACT_DUMP, ACT_N };

    void action_label(int i, char *out, unsigned cap) const {
        switch (i) {
            case ACT_READ:    nova::copy(out, cap, "Read"); break;
            default:          nova::copy(out, cap, "Dump & save"); break;
        }
    }

    void start(int mode, bool save_after) {
        if (g_nfc_live) { nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "busy"); return; }
        g_nfc_sda = board::pin(board::PIN_SDA);
        g_nfc_scl = board::pin(board::PIN_SCL);
        g_nfc_mode = mode;
        g_nfc_save_after = save_after;
        g_nfc_msg[0] = 0;
        g_nfc_done = g_nfc_total = 0;
        g_nfc_run = true;
        if (fw_task_spawn("novanfc", nfc_worker, nullptr, 3072) < 0) {
            g_nfc_run = false;
            nova::copy(g_nfc_msg, sizeof(g_nfc_msg), "No task free");
        }
    }

    void run_action(void) {
        switch (sel_) {
            case ACT_READ: start(NFC_READ, false); break;
            default:       start(NFC_DUMP, true); break;
        }
    }

    int      sel_ = 0, top_ = 0;
    uint32_t phase_ = 0;
    bool     started_ = false;
    bool     wired_ = false;
};

void open_nfc(void) { gui::push<NfcScreen>(); }

}  // namespace screens
}  // namespace nova
