// Desc: The WiFi screens — the link, the scan, the saved networks and the survey.
// File: novagui_wifi.h
//
// A flat sibling of novagui, the same shape as novagui_tools. Everything the
// radio needs is in one file because the screens share one worker task and one
// scan table: two of them scanning at once is two writers on one array, and
// keeping that in a single translation unit is what makes the rule enforceable
// rather than remembered. The device sweep is here for the second half of that
// rule — it holds the link for the best part of a minute, so op_busy() has to
// know about it, and op_busy() lives here.
#ifndef NOVA_GUI_WIFI_H
#define NOVA_GUI_WIFI_H

namespace nova {
namespace screens {

// The App table's open functions, so their shape is fixed by it.
void open_wifi(void);
void open_networks(void);
void open_wardrive(void);
void open_lan(void);      // what else is on the network we joined

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_WIFI_H
