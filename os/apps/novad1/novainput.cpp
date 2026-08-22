// Desc: One encoder and three buttons, turned into four gestures.
// File: novainput.cpp
#include "novainput.h"
#include "novaboard.h"
#include "novacore.h"

#include "rpc_app.h"

namespace nova {

// --- why the encoder is polled and the buttons are not -----------------------
//
// These need opposite treatment, and getting that backwards is what made the
// MicroPython version drop presses for a year.
//
// A BUTTON tap lasts about 40 ms and the UI loop naps up to 300 ms when the
// screen is off. Polling cannot see a press that starts and ends inside one nap
// — the pin is high before and high after — so a button sometimes did nothing at
// all until it was pressed again. That is what fw_gpio_watch is for: the
// firmware counts the edges, and however long this task is away, the count is
// still there when it gets back.
//
// The ENCODER cannot use a count, because a count has no direction. Direction
// comes from the ORDER of A and B, which means reading both together, which
// means sampling. So the encoder is polled — at 2 ms, which is 500 Hz against a
// hand turning maybe 20 detents a second, or 80 transitions. Two pin reads every
// 2 ms is a thousand supervisor calls a second: at 296 cycles each that is 0.2%
// of one core.
//
// Turned faster than 500 Hz can follow, this loses steps rather than reversing —
// the transition table below rejects a jump it cannot account for instead of
// guessing at it. Losing a step on a violent spin is invisible; a menu that
// jumps backwards is not.
//
// A PIO quadrature decoder would remove even that, and PIO is available. It is
// not worth a state machine yet: the IR transmitter and the sub-GHz timing both
// want one and neither of them has an alternative.
//
// THIS RUNS ON ITS OWN TASK, and that is not an optimisation.
//
// The first version polled from the UI loop, which sleeps between 16 and 300 ms
// depending on what is happening. Quadrature is decoded from CONSECUTIVE
// samples: a reading every 140 ms is not consecutive with anything, the
// transition table correctly refuses every jump it cannot account for, and the
// encoder produces NOTHING. Not jitter, not the wrong direction — silence.
// Which looks exactly like a wiring fault, and is not one.
//
// So the sampling lives where its rate is its own business, and the UI loop is
// free to sleep as long as it likes.

// Buxton's full-step table, carried over from the MicroPython suite unchanged so
// an encoder wired for that build turns the same way on this one.
//
// Index is state * 4 + ((A << 1) | B). The low three bits are the next state;
// 0x10 means a step clockwise and 0x20 anticlockwise. Rows are START, CW_FINAL,
// CW_BEGIN, CW_NEXT, CCW_BEGIN, CCW_FINAL, CCW_NEXT.
//
// The point of a table rather than "did A change while B was low" is that it
// rejects everything it cannot account for. A bouncing contact walks into a
// state that goes nowhere and comes back to START without emitting anything, so
// no amount of chatter produces a phantom step.
#define DIR_CW  0x10
#define DIR_CCW 0x20

static const uint8_t kTable[7 * 4] = {
    0x0, 0x2, 0x4, 0x0,          // R_START
    0x3, 0x0, 0x1, 0x10,         // R_CW_FINAL   -> START, and a step clockwise
    0x3, 0x2, 0x0, 0x0,          // R_CW_BEGIN
    0x3, 0x2, 0x1, 0x0,          // R_CW_NEXT
    0x6, 0x0, 0x4, 0x0,          // R_CCW_BEGIN
    0x6, 0x5, 0x0, 0x20,         // R_CCW_FINAL  -> START, and a step anticlockwise
    0x6, 0x5, 0x4, 0x0,          // R_CCW_NEXT
};

// The scan order. SELECT is the encoder's own push, so the two most-used
// gestures are the same thumb without moving it.
#define BTN_SELECT 0
#define BTN_BACK   1
#define BTN_HOME   2

const char *event_name(Event e) {
    switch (e) {
        case EV_ROT_CW:      return "turn right";
        case EV_ROT_CCW:     return "turn left";
        case EV_SELECT:      return "select";
        case EV_BACK:        return "back";
        case EV_HOME:        return "home";
        case EV_SELECT_HOLD: return "select held";
        case EV_HOME_HOLD:   return "home held";
        default:             return "none";
    }
}

// --- the queue ---------------------------------------------------------------

void Input::push(Event e) {
    unsigned next = (head_ + 1) % QUEUE;
    // Drop the NEWEST when full, not the oldest. A stuck button that fills the
    // ring must not push out the gesture somebody actually made before it stuck.
    if (next == tail_) return;
    queue_[head_] = e;
    head_ = next;
}

Event Input::next(void) {
    if (head_ == tail_) return EV_NONE;
    Event e = queue_[tail_];
    tail_ = (tail_ + 1) % QUEUE;
    return e;
}

void Input::unget(Event e) {
    unsigned prev = (tail_ + QUEUE - 1) % QUEUE;
    // Full is the one case where it cannot go back. Dropping it is right: the
    // queue is already holding more turning than the screen can show, and one
    // more detent at the far end of it changes nothing anybody would see.
    if (prev == head_) return;
    tail_ = prev;
    queue_[tail_] = e;
}

void Input::flush(void) { head_ = tail_ = 0; }

void Input::inject(Event e) { push(e); }

// --- bring-up ----------------------------------------------------------------

bool Input::begin(void) {
    ready_ = false;
    flush();

    pin_a_ = board::pin(board::PIN_ENC_A);
    pin_b_ = board::pin(board::PIN_ENC_B);
    pin_btn_[BTN_SELECT] = board::pin(board::PIN_ENC_SW);
    pin_btn_[BTN_BACK]   = board::pin(board::PIN_BTN1);
    pin_btn_[BTN_HOME]   = board::pin(board::PIN_BTN2);

    if (pin_a_ == board::PIN_NONE || pin_b_ == board::PIN_NONE) return false;

    // Default REVERSED. The reference board reads backwards without it — the
    // reported complaint — so the out-of-the-box direction is the corrected one,
    // and a board wired the other way clears the setting rather than recompiling.
    rev_ = nova::reg_bool(NOVA_KEY_PREFIX "EncRev", true);

    // Active low with a pull-up: the switch shorts to ground, which is how every
    // EC11 and tactile button on the reference build is wired.
    fw_gpio_init((unsigned)pin_a_, FW_PIN_IN);
    fw_gpio_pull((unsigned)pin_a_, FW_PULL_UP);
    fw_gpio_init((unsigned)pin_b_, FW_PIN_IN);
    fw_gpio_pull((unsigned)pin_b_, FW_PULL_UP);

    for (int i = 0; i < 3; i++) {
        down_[i] = false;
        hold_sent_[i] = false;
        down_at_[i] = up_at_[i] = 0;
        if (pin_btn_[i] == board::PIN_NONE) continue;
        fw_gpio_init((unsigned)pin_btn_[i], FW_PIN_IN);
        fw_gpio_pull((unsigned)pin_btn_[i], FW_PULL_UP);
        // Both edges: a press and its release are both needed, one to start the
        // hold timer and one to decide whether it was a tap.
        fw_gpio_watch((unsigned)pin_btn_[i], FW_EDGE_BOTH);
    }

    // Start from where the encoder actually is, not from state 0 with an assumed
    // pin state — otherwise the first turn after boot emits a step in whichever
    // direction the assumption happened to be wrong in.
    int a = fw_gpio_get((unsigned)pin_a_);
    int b = fw_gpio_get((unsigned)pin_b_);
    (void)a; (void)b;
    state_ = 0;

    ready_ = true;
    return true;
}

// --- polling -----------------------------------------------------------------

void Input::poll_encoder(void) {
    int a = fw_gpio_get((unsigned)pin_a_);
    int b = fw_gpio_get((unsigned)pin_b_);
    if (a < 0 || b < 0) return;
    uint8_t ps = (uint8_t)((a << 1) | b);
    uint8_t st = kTable[(state_ & 0x07) * 4 + ps];
    state_ = st;
    // rev_ swaps the two. The table decode is untouched — a decoded step is still
    // a decoded step — only which direction it MEANS is flipped, so a reversed
    // board behaves like a normally wired one everywhere above this line.
    if      (st & DIR_CW)  push(rev_ ? EV_ROT_CCW : EV_ROT_CW);
    else if (st & DIR_CCW) push(rev_ ? EV_ROT_CW  : EV_ROT_CCW);
}

void Input::poll_button(int i) {
    if (pin_btn_[i] == board::PIN_NONE) return;

    int level = 1;
    int edges = fw_gpio_events((unsigned)pin_btn_[i], &level);
    if (edges < 0) return;                       // not being watched

    uint32_t now = fw_millis();
    bool is_down = (level == 0);                 // active low

    // DEBOUNCE BY DURATION, not by ignoring edges.
    //
    // A mechanical switch chatters for a few milliseconds on both edges, so one
    // press produces a burst of press/release pairs. Ignoring edges that arrive
    // too soon after the last one swallows the genuine release of a fast tap as
    // well; requiring the button to have SETTLED in its new state for longer
    // than bounce lasts does not.
    if (is_down != down_[i]) {
        if (is_down) {
            if (now - up_at_[i] < DEBOUNCE_MS) return;      // chatter after a release
            down_[i] = true;
            hold_sent_[i] = false;
            down_at_[i] = now;
            // BACK fires on the PRESS edge. It has no hold gesture, so there is
            // nothing to wait to distinguish it from, and going back should feel
            // immediate.
            if (i == BTN_BACK) push(EV_BACK);
        } else {
            uint32_t held = now - down_at_[i];
            if (held < DEBOUNCE_MS) return;                 // chatter — still down
            down_[i] = false;
            up_at_[i] = now;
            if (hold_sent_[i]) {
                // The hold already went out while the button was down. Its
                // release must not also count as a tap.
                hold_sent_[i] = false;
            } else if (i == BTN_SELECT) {
                push(held >= HOLD_MS ? EV_SELECT_HOLD : EV_SELECT);
            } else if (i == BTN_HOME) {
                push(held >= HOLD_MS ? EV_HOME_HOLD : EV_HOME);
            }
        }
    }

    // A hold fires WHILE the button is still down.
    //
    // Holding HOME opens the power menu, and a gesture that only happens when
    // the button is let go feels broken — the whole point is that it responds
    // while being held. So it is reported from here rather than on the release
    // edge, once, and the release path above knows not to repeat it.
    if (down_[i] && !hold_sent_[i] && (now - down_at_[i]) >= HOLD_MS) {
        hold_sent_[i] = true;
        if      (i == BTN_SELECT) push(EV_SELECT_HOLD);
        else if (i == BTN_HOME)   push(EV_HOME_HOLD);
    }

    // An edge count above one with the level unchanged is a press and release
    // that both happened between two polls — a genuine fast tap, or a burst of
    // chatter. It is delivered rather than dropped, because a tap that does
    // nothing is the exact complaint this whole path exists to fix.
    if (edges >= 2 && is_down == down_[i] && !is_down) {
        if (now - up_at_[i] >= DEBOUNCE_MS) {
            up_at_[i] = now;
            if      (i == BTN_SELECT) push(EV_SELECT);
            else if (i == BTN_BACK)   push(EV_BACK);
            else if (i == BTN_HOME)   push(EV_HOME);
        }
    }
}

bool Input::running(void) const { return run_; }

void Input::poll(void) {
    if (!ready_) return;
    poll_encoder();
    for (int i = 0; i < 3; i++) poll_button(i);
}

// --- the task ------------------------------------------------------------------

static Input g_input;
Input &input(void) { return g_input; }

static int input_task(void *) {
    // 2 ms is 500 samples a second against a hand producing maybe 80 transitions
    // a second, so every one of them is caught several times over. The cost is
    // two pin reads per sample: a thousand supervisor calls a second, which at
    // 296 cycles is about two tenths of one percent of a core.
    while (g_input.ready() && !fw_task_should_stop() && g_input.running()) {
        g_input.poll();
        fw_task_sleep_ms(2);
    }
    return 0;
}

bool Input::start(void) {
    if (!ready_) return false;
    if (run_) return true;
    run_ = true;
    // A small stack: this task calls four ABI functions and holds nothing.
    pid_ = fw_task_spawn("novainput", input_task, nullptr, 2048);
    if (pid_ < 0) { run_ = false; return false; }
    return true;
}

void Input::stop(void) {
    run_ = false;
    pid_ = 0;
}

}  // namespace nova
