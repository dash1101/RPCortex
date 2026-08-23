// Desc: One encoder and three buttons, turned into four gestures.
// File: novainput.h
//
// The whole control budget of the device: turn, SELECT, HOME, BACK. Every screen
// speaks this vocabulary and nothing above this file ever reads a pin.
//
// The meanings are fixed across the entire UI, which is how somebody learns the
// device rather than learning fifty screens:
//
//   turn      move the selection, or adjust the focused value. Never destructive.
//   SELECT    activate whatever the cursor is on.
//   HOME      go to the home screen. Held, it opens the power menu from anywhere.
//   BACK      up one level, or cancel. Must ALWAYS work and always mean back —
//             a screen that traps someone is a bug, not a design.
#ifndef NOVA_INPUT_H
#define NOVA_INPUT_H

#include <stdint.h>

namespace nova {

enum Event {
    EV_NONE = 0,
    EV_ROT_CW,
    EV_ROT_CCW,
    EV_SELECT,
    EV_BACK,
    EV_HOME,
    EV_SELECT_HOLD,     // a shortcut, e.g. "done" in the keyboard
    EV_HOME_HOLD,       // the power menu, from anywhere
};

const char *event_name(Event e);

// How long a press has to last to count as a hold, and how long contact chatter
// is allowed to be. Both carried over from the MicroPython suite, where they
// were arrived at on real switches rather than from a datasheet.
constexpr uint32_t HOLD_MS     = 600;
constexpr uint32_t DEBOUNCE_MS = 25;

class Input {
public:
    // Claim the pins. False when the board profile has no encoder configured,
    // which is a device somebody has not finished wiring rather than a fault.
    bool begin(void);
    bool ready(void) const { return ready_; }

    // Sample the pins and fold what happened into the queue. Called by the input
    // TASK every two milliseconds — see the note in the .cpp about why this is
    // polled and the buttons are not, and why it cannot ride the UI loop.
    void poll(void);

    // Start and stop the task that does the polling. The encoder does not work
    // without it, at all: quadrature is decoded from CONSECUTIVE samples, and a
    // sample every 140 ms is not consecutive with anything.
    bool start(void);
    void stop(void);
    bool running(void) const;

    // The next gesture, or EV_NONE. Never blocks.
    Event next(void);

    // Put one back at the FRONT, so it is the next one out. The runner takes a
    // single rotation step per frame and returns the rest; a plain push would
    // put them behind whatever arrived since and reverse the order of a spin.
    void unget(Event e);

    // Is anything waiting? For the UI loop's decision about how long to nap.
    bool pending(void) const { return head_ != tail_; }

    // Drop everything queued. Used when a screen takes over — the keyboard, the
    // lock screen — so gestures aimed at the screen underneath do not arrive
    // somewhere they were not meant for.
    void flush(void);

    // For the scripted source below and for tests: push an event as though the
    // hardware had produced it.
    void inject(Event e);

    // Reverse the encoder. Which way "clockwise" moves the selection depends on
    // how the two phases are wired, and it has flip-flopped between builds, so it
    // is a SETTING (Apps.NovaD1_EncRev) rather than a hard-coded direction that
    // gets flipped again next time. begin() reads it; the Display settings row
    // toggles it live through here, so a board wired the other way is a preference
    // and not a recompile. This only swaps which event a decoded step emits —
    // which physical direction is correct is a bench check on the
    // knob, because a host cannot: injected gestures are post-decode.
    void set_reversed(bool on) { rev_ = on; }
    bool reversed(void) const { return rev_; }

private:
    bool     ready_;

    // The encoder.
    int      pin_a_, pin_b_;
    uint8_t  state_;            // the transition table's current state
    bool     rev_;             // swap CW/CCW — see set_reversed()

    // The buttons, in a fixed scan order so an index means the same thing
    // everywhere: 0 = SELECT (the encoder's own push), 1 = BACK, 2 = HOME.
    int      pin_btn_[3];
    bool     down_[3];
    bool     hold_sent_[3];     // this press already produced its hold
    uint32_t down_at_[3];
    uint32_t up_at_[3];

    // A ring, because a burst of turns between two polls is normal and dropping
    // the middle of it makes a menu feel like it is sticking.
    // Written by the input task, read by the UI task, possibly on the other
    // core. head_ has one writer and tail_ has one writer, which makes the ring
    // safe without a lock — but both must be volatile, or the compiler is free
    // to cache the other side's index across a loop and the queue appears never
    // to change.
    static constexpr unsigned QUEUE = 32;
    Event             queue_[QUEUE];
    volatile unsigned head_, tail_;

    int      pid_;
    volatile bool run_;

    void push(Event e);
    void poll_encoder(void);
    void poll_button(int i);
};

Input &input(void);

}  // namespace nova

#endif  // NOVA_INPUT_H
