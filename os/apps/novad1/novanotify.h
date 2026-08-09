// Desc: The notification queue, the unread count, and the alert it makes.
// File: novanotify.h
//
// One queue shared by everything that wants to tell the user something while
// they are looking at a different screen. The status bar carries the unread
// count; the Alerts app is the queue.
//
// The haptic and the LED are both GUARDED: neither fires unless its pin is
// actually configured. A buzzer command sent to an unwired pin is silent, which
// is fine, but a vibration command to a pin somebody used for something else is
// not — so nothing is driven that was not asked for.
#ifndef NOVA_NOTIFY_H
#define NOVA_NOTIFY_H

namespace nova {
namespace notify {

constexpr int MAX = 20;
constexpr unsigned TEXT_MAX = 48;

// Add a notification. Also raises the alert, unless alerts are switched off.
void post(const char *text);

// Read one back, newest first.
bool at(int i, char *out, unsigned cap);
int  count(void);
int  unread(void);

// Everything has been seen. Called when the Alerts app opens.
void mark_read(void);

// The newest message not yet shown as a toast, once — true when it filled `out`.
// The runner banners it over the current screen; the status count and Alerts app
// remain the record.
bool take_toast(char *out, unsigned cap);
void clear(void);

// The alert on its own, for a screen that wants a confirmation buzz without a
// queue entry.
void alert(void);

}  // namespace notify
}  // namespace nova

#endif  // NOVA_NOTIFY_H
