// The USB keyboard, and the mode that keeps it and the drive apart.
//
// Two things live here. One is the small piece of state that says which USB
// function is active — CONSOLE, STORAGE or HID — and the rule that refuses to
// have the drive and the keyboard on at once. The other is a keystroke queue
// with a producer on the shell task (the `hid` and `badusb` commands, filling
// it) and a consumer on the usb task (draining it into HID reports).
//
// The split is not tidiness, it is correctness. A HID report may only be
// submitted from core 0, the core the USB device stack runs on. TinyUSB guards
// its state by disabling the USB interrupt, and on this part that disables it on
// the calling core only (see the long note in usbdev.cpp). Submitting a report
// from the shell task, which may be on core 1, would disable an interrupt that
// is enabled on the other core while that other core is halfway through the same
// state — the same class of bug that pins the usb task and bound cyw43 to its
// init core. So the shell task never touches the device stack: it enqueues, and
// the usb task's service turn does the rest.

#include "usbhid.h"

#include "ducky.h"
#include "hidkey.h"
#include "lock.h"
#include "task.h"

#include <string.h>

#include "tusb.h"

// --- the active mode --------------------------------------------------------

// Serialises usb_mode_enter/leave. Both run on the shell task (the `download`,
// `hid` and `badusb` commands, and the GUI launcher via a detached shell), and
// two of them arriving together is what the lock is for. The usb task only
// READS the mode, without the lock — a one-turn-stale read just gates a
// keystroke, which is harmless.
static RpcLock g_mode_lock;
// CONSOLE is usbmode_boot(); a keyboard present but idle types nothing until an
// explicit command switches HID on.
static volatile UsbMode g_usb_mode = USB_MODE_CONSOLE;

// --- the keystroke queue (single producer, single consumer) -----------------
//
// The shell task writes g_head; the usb task writes g_tail and the "in flight"
// state below. Nothing else writes either side, which is what makes this safe
// without a lock in the hot path — a lock there would be taken on both cores
// around the device stack, the very inversion the drive was rebuilt to avoid.

enum { ACT_KEY = 0, ACT_DELAY = 1, ACT_HOLD = 2, ACT_RELEASE = 3 };
struct HidAction { uint8_t type; uint8_t mod; uint8_t kc; uint32_t ms; };

#define HID_RING 64                    // one slot always left empty (full = head+1==tail)
static volatile HidAction g_ring[HID_RING];
static volatile uint16_t  g_head;      // producer (shell task)
static volatile uint16_t  g_tail;      // consumer (usb task)
static volatile bool      g_abort;     // producer raises it; consumer drops the queue

// Consumer-owned, read by the producer's flush.
static volatile uint8_t   g_key_down;  // a transient key is pressed, release it next turn
static volatile bool      g_delaying;
static volatile uint32_t  g_delay_until;

// The HELD set — what HOLD put down and RELEASE has not yet let go of. A boot
// keyboard report carries at most six keycodes; five of the slots can be held
// so one is always free for the transient key that a normal keystroke taps on
// top of them. Consumer-owned, like g_key_down.
#define HID_HELD_MAX 5
static volatile uint8_t   g_held_mod;          // modifiers held down
static volatile uint8_t   g_held_keys[HID_HELD_MAX];
static volatile uint8_t   g_held_n;

#if CFG_TUD_HID
// One report: every held key and modifier, plus an optional transient key
// tapped on top. This is the single place a report is built, so held state can
// never be lost — a normal keystroke, a HOLD, and a RELEASE all send through
// here and all carry whatever is currently held.
static void report_now(uint8_t xtra_mod, uint8_t xtra_kc) {
    uint8_t keys[6] = { 0, 0, 0, 0, 0, 0 };
    int i = 0;
    for (int j = 0; j < g_held_n && i < 6; j++) keys[i++] = g_held_keys[j];
    if (xtra_kc && i < 6) keys[i++] = xtra_kc;
    tud_hid_keyboard_report(0, (uint8_t)(g_held_mod | xtra_mod), keys);
}
static inline bool hid_ready(void) { return tud_hid_ready(); }
#else
static void report_now(uint8_t, uint8_t) {}
static inline bool hid_ready(void) { return false; }
#endif

// Add a key to the held set (idempotent, and silently full-safe). Consumer-side.
static void held_add(uint8_t kc) {
    if (!kc) return;
    for (int j = 0; j < g_held_n; j++) if (g_held_keys[j] == kc) return;
    if (g_held_n < HID_HELD_MAX) g_held_keys[g_held_n++] = kc;
}
// Remove one held key, or (kc==0) leave the set alone.
static void held_remove(uint8_t kc) {
    if (!kc) return;
    for (int j = 0; j < g_held_n; j++) {
        if (g_held_keys[j] == kc) {
            for (int k = j + 1; k < g_held_n; k++) g_held_keys[k - 1] = g_held_keys[k];
            g_held_n--;
            return;
        }
    }
}
static void held_clear(void) { g_held_mod = 0; g_held_n = 0; }

// --- the consumer: run on the usb task, core 0 ------------------------------

void usbhid_service(void) {
    // Not the active mode: make sure no key is left held, forget anything queued,
    // and do nothing else. This also covers leaving HID mid-payload — the last
    // release is sent here.
    if (g_usb_mode != USB_MODE_HID) {
        // Leaving HID: let go of everything still held and stop. The last report
        // is an all-keys-up, so a payload interrupted mid-HOLD does not leave a
        // key stuck down on the host.
        if (g_key_down || g_held_n || g_held_mod) {
            if (hid_ready()) { g_key_down = 0; held_clear(); report_now(0, 0); }
        } else { g_tail = g_head; g_delaying = false; }
        return;
    }

    // Interrupted: drop the rest of the payload and release everything held.
    if (g_abort) {
        g_tail = g_head;
        g_delaying = false;
        if (g_key_down || g_held_n || g_held_mod) {
            if (hid_ready()) { g_key_down = 0; held_clear(); report_now(0, 0); }
        }
        return;
    }

    // A transient press is always followed by its release before the next
    // action, so the host sees two distinct keystrokes for two identical
    // characters. The release report still carries whatever is HELD.
    if (g_key_down) {
        if (hid_ready()) { report_now(0, 0); g_key_down = 0; }
        return;
    }

    // An in-stream delay pauses the typing without pausing the device.
    if (g_delaying) {
        if ((int32_t)(task_now_ms() - g_delay_until) < 0) return;
        g_delaying = false;
    }

    if (g_tail == g_head) return;      // nothing queued

    // Read the slot field by field: a whole-struct copy out of a volatile array
    // is ill-formed, and reading the members is what actually wants to be
    // volatile anyway.
    HidAction a;
    a.type = g_ring[g_tail].type;
    a.mod  = g_ring[g_tail].mod;
    a.kc   = g_ring[g_tail].kc;
    a.ms   = g_ring[g_tail].ms;
    if (a.type == ACT_DELAY) {
        g_delay_until = task_now_ms() + a.ms;
        g_delaying = (a.ms > 0);
        g_tail = (uint16_t)((g_tail + 1) % HID_RING);
        return;
    }

    if (!hid_ready()) return;          // host not reading yet; the queue backs up

    if (a.type == ACT_HOLD) {
        g_held_mod = (uint8_t)(g_held_mod | a.mod);
        held_add(a.kc);
        report_now(0, 0);              // hold takes effect now, with no transient
        g_tail = (uint16_t)((g_tail + 1) % HID_RING);
        return;
    }
    if (a.type == ACT_RELEASE) {
        if (!a.mod && !a.kc) held_clear();
        else { g_held_mod = (uint8_t)(g_held_mod & ~a.mod); held_remove(a.kc); }
        report_now(0, 0);
        g_tail = (uint16_t)((g_tail + 1) % HID_RING);
        return;
    }

    // A key: press it now (on top of anything held), release it next turn.
    report_now(a.mod, a.kc);
    g_key_down = 1;
    g_tail = (uint16_t)((g_tail + 1) % HID_RING);
}

// --- the producer: run on the shell task ------------------------------------

static void ring_push(uint8_t type, uint8_t mod, uint8_t kc, uint32_t ms,
                      int (*stop)(void)) {
    for (;;) {
        if (g_abort) return;
        uint16_t next = (uint16_t)((g_head + 1) % HID_RING);
        if (next != g_tail) {
            g_ring[g_head].type = type;
            g_ring[g_head].mod  = mod;
            g_ring[g_head].kc   = kc;
            g_ring[g_head].ms   = ms;
            __sync_synchronize();       // publish the slot before advancing head
            g_head = next;
            return;
        }
        // Full: let the consumer catch up, or give up if asked.
        if (stop && stop()) { g_abort = true; return; }
        task_yield();
    }
}

static void enqueue_key(uint8_t mod, uint8_t kc, int (*stop)(void)) {
    ring_push(ACT_KEY, mod, kc, 0, stop);
}
static void enqueue_delay(uint32_t ms, int (*stop)(void)) {
    ring_push(ACT_DELAY, 0, 0, ms, stop);
}
static void enqueue_hold(uint8_t mod, uint8_t kc, int (*stop)(void)) {
    ring_push(ACT_HOLD, mod, kc, 0, stop);
}
static void enqueue_release(uint8_t mod, uint8_t kc, int (*stop)(void)) {
    ring_push(ACT_RELEASE, mod, kc, 0, stop);
}
static int enqueue_string(const char *s, int (*stop)(void)) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (stop && stop()) { g_abort = true; break; }
        uint8_t kc = 0, sh = 0;
        if (!hid_ascii_to_keycode((char)*p, &kc, &sh)) continue;   // skip what a keyboard cannot type
        enqueue_key(sh ? HID_MOD_SHIFT : 0, kc, stop);
        n++;
    }
    return n;
}

// ALTSTRING: type each character by its decimal code on the number pad while
// Alt is held — the Windows "Alt code" trick, which a host turns into the
// character independent of its keyboard layout. Hold Alt, tap the keypad digits
// of the ASCII value, release Alt, and the character appears on release.
static int enqueue_altstring(const char *s, int (*stop)(void)) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (stop && stop()) { g_abort = true; break; }
        unsigned code = (unsigned)*p;             // the ASCII value to spell out
        char digits[6];
        int d = 0;
        if (code == 0) digits[d++] = '0';
        else for (unsigned v = code; v && d < (int)sizeof(digits); v /= 10)
            digits[d++] = (char)('0' + (v % 10));
        enqueue_hold(HID_MOD_ALT, 0, stop);       // Alt down
        for (int i = d - 1; i >= 0; i--)          // digits most-significant first
            enqueue_key(0, hid_digit_to_keypad(digits[i] - '0'), stop);
        enqueue_release(0, 0, stop);              // Alt up -> the character lands
        n++;
    }
    return n;
}

int usbhid_type_text(const char *text, int (*stop)(void)) {
    if (!text) return 0;
    return enqueue_string(text, stop);
}

// The DuckyScript reader calls back with what to type; here that becomes queue
// entries. The parser is in core/ducky.cpp and is host-tested on its own.
struct DuckyCtx { int (*stop)(void); };
static void d_key(void *ctx, uint8_t mod, uint8_t kc) { enqueue_key(mod, kc, ((DuckyCtx *)ctx)->stop); }
static void d_text(void *ctx, const char *s)          { enqueue_string(s, ((DuckyCtx *)ctx)->stop); }
static void d_delay(void *ctx, uint32_t ms)           { enqueue_delay(ms, ((DuckyCtx *)ctx)->stop); }
static int  d_stop(void *ctx) { DuckyCtx *c = (DuckyCtx *)ctx; return c->stop ? c->stop() : 0; }
static void d_hold(void *ctx, uint8_t mod, uint8_t kc)    { enqueue_hold(mod, kc, ((DuckyCtx *)ctx)->stop); }
static void d_release(void *ctx, uint8_t mod, uint8_t kc) { enqueue_release(mod, kc, ((DuckyCtx *)ctx)->stop); }
static void d_altstring(void *ctx, const char *s)        { enqueue_altstring(s, ((DuckyCtx *)ctx)->stop); }

int usbhid_run_ducky(const char *script, int (*stop)(void), int *error_line) {
    DuckyCtx c = { stop };
    DuckyEmit e;
    e.key = d_key; e.text = d_text; e.delay = d_delay; e.stop = d_stop;
    e.hold = d_hold; e.release = d_release; e.altstring = d_altstring; e.ctx = &c;
    DuckyState st;
    int lines = ducky_run(script, &st, &e);
    if (error_line) *error_line = st.error_line;
    return lines;
}

void usbhid_flush(int (*stop)(void)) {
    for (;;) {
        if (g_abort) return;
        if (g_head == g_tail && !g_key_down && !g_delaying) return;
        if (stop && stop()) { g_abort = true; return; }
        task_yield();
    }
}

// --- the mode gate ----------------------------------------------------------

UsbMode usb_mode_current(void) { return g_usb_mode; }

bool usb_mode_enter(UsbMode m) {
    LockGuard _lk(&g_mode_lock);
    if (!usbmode_can_enter(g_usb_mode, m)) return false;
    if (m == USB_MODE_HID) {
        // Arm an empty queue before the service turn can see HID as active, so a
        // stale action from a previous run cannot be typed into this one. The
        // held set starts empty too — no key from a previous session is down.
        g_head = g_tail = 0;
        g_key_down = 0;
        g_delaying = false;
        g_abort = false;
        held_clear();
        __sync_synchronize();
    }
    g_usb_mode = m;
    return true;
}

void usb_mode_leave(UsbMode m) {
    LockGuard _lk(&g_mode_lock);
    // Only the mode that is actually active may stand down — a late leave from a
    // session that already ended must not cancel a newer one.
    if (g_usb_mode == m) g_usb_mode = USB_MODE_CONSOLE;
}
