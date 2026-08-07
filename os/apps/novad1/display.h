// Desc: The OLED panel — one interface, three controllers, a page-diff push.
// File: display.h
//
// The UI never learns which panel it is. Every controller quirk lives here: the
// SH1106's two-column offset, the SSD1309's command unlock, the SSD1306's charge
// pump. A quirk that leaks into a screen is a quirk that has to be remembered by
// everyone who writes one.
//
// THESE PANELS CANNOT BE TOLD APART IN SOFTWARE. They share an I2C address and
// answer nothing that distinguishes them, so the panel is a configuration choice
// and never a guess — `d1 display sh1106`.
//
// The DEFAULT IS THE SH1106, because that is the panel that has actually lit up.
// It was changed to the SSD1309 once — the bill of materials specifies one, and
// a document seemed like a better authority than a default nobody had written
// down. The board went dark for four versions.
//
// The mechanism is worth knowing, because it fails in total silence. An SH1106
// turns its own charge pump on with 0xad 0x8b; the SSD1309 has no pump command
// at all, being driven externally. So an SH1106 sent the SSD1309 sequence has no
// supply to its panel — while every I2C write is acknowledged, nothing reports
// an error, and the screen is simply black.
//
// `novad1 display test` cycles all three with a pattern on each. That is the
// only honest way to identify one of these: ask the person who can see it.
//
// A wrong init sequence FAILS SILENTLY: a blank or garbled panel and no error
// anywhere. The sequences here are carried over byte for byte from the
// MicroPython suite, where the SH1106 is verified on hardware.
#ifndef NOVA_DISPLAY_H
#define NOVA_DISPLAY_H

#include <stdint.h>
#include "novacanvas.h"

namespace nova {

enum PanelKind {
    PANEL_AUTO = 0,     // means SH1106, the one known to work; see above
    PANEL_SH1106,
    PANEL_SSD1306,
    PANEL_SSD1309,
};

class Display {
public:
    // Bring the panel up on the configured I2C pins. Returns false when nothing
    // answered, which is the common first-build case and has to be reported
    // rather than left as a device that boots to a dark screen.
    bool begin(void);

    bool ready(void) const { return ready_; }
    uint8_t address(void) const { return addr_; }
    PanelKind kind(void) const { return kind_; }
    const char *kind_name(void) const;

    // Push what changed. Only the 128-byte pages that actually differ from the
    // last frame are written — most updates touch one or two of the eight, and a
    // full push is the largest single block of I2C the device ever does.
    void show(const Canvas &c);

    // Force the next show() to push every page. Needed after anything that
    // changes the panel's own state, since the diff is against what was SENT and
    // the panel may no longer be holding it.
    void invalidate(void) { have_last_ = false; }

    void contrast(uint8_t value);
    void power(bool on);
    void invert(bool on);

    // Bring the panel up as a NAMED controller, ignoring what is configured.
    // For `novad1 display test`, which is the only honest way to identify one of
    // these: they share an I2C address and answer nothing that tells them apart,
    // so the question goes to the person who can see the screen.
    bool begin_as(PanelKind kind);

    // How many pages the last show() actually wrote, for the frame-rate readout.
    // The number that says whether the diff is earning its keep.
    unsigned last_pages(void) const { return last_pages_; }

    // NO DEFAULT MEMBER INITIALISERS ANYWHERE IN THIS CLASS, and the same goes
    // for anything else this package holds as a static.
    //
    // A member initialiser makes the default constructor non-trivial, and an
    // object with a non-trivial constructor held at namespace scope needs
    // .init_array to run before it is valid. The loader does not run
    // .init_array — so the constructor would simply never happen, and every
    // field would be whatever zero-initialisation left it. Which is fine here,
    // and would not be for a class that expected its constructor to have run.
    //
    // So this is a plain object in bss that starts as zeroes, and begin() sets
    // everything that is not zero. tools_checkapp.py refuses any package that
    // emits a .init_array at all, so this cannot be got wrong quietly.
private:
    bool      ready_;
    uint8_t   addr_;
    unsigned  bus_;
    PanelKind kind_;
    int       col_offset_;
    unsigned  last_pages_;

    // The previous frame, for the diff. 1 KB of bss rather than an allocation:
    // it lives exactly as long as the package does, and taking it from the 12 KB
    // arena would be most of the arena.
    bool      have_last_;
    PanelKind forced_;      // begin_as(), for the identify test
    unsigned  fails_;       // consecutive frames the panel would not take
    uint8_t   last_[128 * 8];

    bool cmds(const uint8_t *seq, unsigned n);
    bool probe(uint8_t addr);
    void note_fail(void);
};

// The one display the device has. A singleton because the panel is: two Display
// objects would each keep their own idea of what the panel is showing and the
// diff would be wrong for both.
Display &display(void);

// Parse a panel name, for `d1 display <kind>` and the registry.
PanelKind panel_from_name(const char *s);

}  // namespace nova

#endif  // NOVA_DISPLAY_H
