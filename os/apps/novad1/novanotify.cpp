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

int count(void)  { return g_count; }
int unread(void) { return g_unread; }
void mark_read(void) { g_unread = 0; }

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

    // Everything notified is also logged. The queue is what is happening; the
    // log is what happened, and the second one is what somebody reads when they
    // are trying to work out why.
    log::write(text);

    alert();
}

}  // namespace notify
}  // namespace nova
