// Desc: The notification queue, the unread count, and the alert it makes.
// File: novanotify.cpp
#include "novanotify.h"
#include "novacore.h"
#include "novaboard.h"
#include "novalog.h"

#include "rpc_app.h"
#include <string.h>

namespace nova {
namespace notify {

// A ring in bss. Twenty entries of forty-eight characters is under a kilobyte,
// and it does not persist across a reboot on purpose: a notification is about
// something happening NOW, and a queue of yesterday's is a chore rather than
// information. What is worth keeping goes to the log, which does persist.
static char     g_text[MAX][TEXT_MAX];
static unsigned g_head;         // where the next one goes
static int      g_count;
static int      g_unread;

// The newest message, waiting to be shown as a toast — the runner takes it once
// and banners it over whatever screen is up, so a notification is seen from
// anywhere without the app underneath being disturbed.
static char     g_toast[TEXT_MAX];
static bool     g_toast_pending;

int count(void)  { return g_count; }
int unread(void) { return g_unread; }
void mark_read(void) { g_unread = 0; }

// The newest message not yet shown as a toast, once. The runner calls this each
// frame; a true return starts a banner.
bool take_toast(char *out, unsigned cap) {
    if (!g_toast_pending || !out || !cap) return false;
    nova::copy(out, cap, g_toast);
    g_toast_pending = false;
    return true;
}

void clear(void) {
    g_head = 0;
    g_count = 0;
    g_unread = 0;
}

bool at(int i, char *out, unsigned cap) {
    if (i < 0 || i >= g_count || !out || !cap) return false;
    // Newest first: index 0 is the one just before head.
    unsigned slot = (g_head + MAX - 1 - (unsigned)i) % MAX;
    nova::copy(out, cap, g_text[slot]);
    return true;
}

void alert(void) {
    if (!nova::reg_bool(NOVA_KEY_PREFIX "Notify", true)) return;

    // The motor first, because it is the one that gets noticed in a pocket, and
    // only if a pin was actually set for it.
    if (nova::reg_bool(NOVA_KEY_PREFIX "Notify_Haptic", true)) {
        int vibe = board::pin(board::PIN_VIBE);
        if (vibe != board::PIN_NONE) {
            fw_gpio_init((unsigned)vibe, FW_PIN_OUT);
            fw_gpio_put((unsigned)vibe, 1);
            // Short and blocking. A motor left running because whoever was going
            // to switch it off did not get scheduled is a device that buzzes
            // until the battery goes, so this does not hand the job to anyone.
            fw_busy_wait_us(60000);
            fw_gpio_put((unsigned)vibe, 0);
        }

        int buz = board::pin(board::PIN_BUZZER);
        if (buz != board::PIN_NONE) {
            // Two rising chirps. One is a beep; two at different pitches reads
            // as deliberate, which is what tells it apart from a fault.
            static const unsigned kTone[2] = { 1800, 2400 };
            for (int i = 0; i < 2; i++) {
                fw_pwm_init((unsigned)buz, kTone[i]);
                fw_pwm_duty((unsigned)buz, 500);
                fw_busy_wait_us(50000);
            }
            fw_pwm_stop((unsigned)buz);
        }
    }

    if (nova::reg_bool(NOVA_KEY_PREFIX "Notify_LED", true)) {
        int led = board::pin(board::PIN_LED);
        // No fallback to the board's own LED. On Pico W-class boards it hangs
        // off the wireless module and is not a GPIO at all, so there is nothing
        // to drive unless somebody wired one and said which pin.
        if (led != board::PIN_NONE) {
            fw_gpio_init((unsigned)led, FW_PIN_OUT);
            for (int i = 0; i < 2; i++) {
                fw_gpio_put((unsigned)led, 1);
                fw_busy_wait_us(40000);
                fw_gpio_put((unsigned)led, 0);
                fw_busy_wait_us(40000);
            }
        }
    }
}

void post(const char *text) {
    if (!text || !*text) return;

    nova::copy(g_text[g_head], TEXT_MAX, text);
    g_head = (g_head + 1) % MAX;
    if (g_count < MAX) g_count++;
    if (g_unread < MAX) g_unread++;

    // Queue it for a toast, unless notifications are switched off — the same
    // master setting that silences the buzz and the LED. The queue above still
    // records it for the Alerts app; this is only the banner. The newest wins if
    // two arrive before the runner looks: a banner is a glance, not a transcript.
    if (nova::reg_bool(NOVA_KEY_PREFIX "Notify", true)) {
        nova::copy(g_toast, TEXT_MAX, text);
        g_toast_pending = true;
    }

    // Everything notified is also logged. The queue is what is happening; the
    // log is what happened, and the second one is what somebody reads when they
    // are trying to work out why.
    log::write(text);

    alert();
}

}  // namespace notify
}  // namespace nova
