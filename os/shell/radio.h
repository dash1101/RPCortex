// The wireless chip, and who may use it.
//
// Wi-Fi (net.cpp) and Bluetooth (bt.cpp, btaudio.cpp) are two radios on ONE
// CYW43, behind one SPI link and one async_context. Whichever of them needs the
// chip first brings it up, through radio_up(), and both live under the same
// one-owner rule that netown.h states. That part lives in radio.cpp so that an
// image can carry either radio without the other; it used to sit at the top of
// net.cpp, which made Bluetooth impossible to build without Wi-Fi.
//
// radio.cpp is compiled on the wireless boards when Wi-Fi or Bluetooth is in the
// image. radio_off.cpp stands in for it otherwise: nothing to own, nothing to
// lock, and a `radio` command that says why.
#ifndef RPC_RADIO_H
#define RPC_RADIO_H

#include <stdint.h>
#include "lock.h"
#include "blackbox.h"

// The network operation lock: ONE task inside cyw43 or lwIP at a time. See
// netown.h for the rule and NetOwn for the usual way to take it. net.cpp takes
// it directly in the few places where the migration NetOwn does is not wanted.
extern RpcLock g_net_op;

// Whether the chip has been powered up. Written only by radio_up and radio_down.
extern bool g_radio_up;

// The privacy latch, `radio off`. Every path that brings the chip up checks it.
bool radio_locked(void);

// Bring the chip up if it is not already (an ACTIVATOR: it loads the firmware
// blob), or take it down. Both expect the caller to own the radio.
bool radio_up(void);
void radio_down(void);

// The same two, under the names the other files have always used. net_radio_up
// is radio_up; net_radio_is_up is a pure question and never touches the chip.
bool net_radio_up(void);
bool net_radio_is_up(void);
// Raised each time the radio goes down; see netown.h.
uint32_t net_radio_gen(void);

// `radio` and its alias `airplane`.
void radio_register(void);

// What the Wi-Fi layer does as the chip comes up and goes down: station mode,
// the power-save setting and the first status reading on the way up; closing
// every connection and then clearing the status cache on the way down.
// net.cpp defines them. Bluetooth needs Wi-Fi (tools/features.py), so
// radio.cpp is only ever built beside net.cpp.
bool wifi_radio_starting(void);    // after cyw43_arch_init, before the chip is marked up;
                                   // false: the chip did not start (and is down again)
void wifi_radio_started(void);     // after it is marked up, on the radio's core
void wifi_radio_stopping(void);    // before cyw43_arch_deinit: close every connection
void wifi_radio_stopped(void);     // after cyw43_arch_deinit

// Checkpoints through the radio bring-up and join.
//
// Every one of these steps is a call INTO the driver that does not yield, so if
// one of them never returns the device hangs with nothing printed — the USB
// buffer dies with it. bb_note_phase writes to memory the reset does not clear,
// so the next boot can say which call it was.
//
// Worth knowing what these are NOT. The phase is one slot shared with every
// package, and fw_millis writes it on every call, so on a device that is
// drawing anything these are overwritten within a millisecond of being set —
// by the OTHER core, which is still running, because only one core has stopped.
// They are the answer on a quiet device and nothing at all on a busy one. The
// fields that survive a running package are the ones in BlackBox with a single
// writer each; max_stall_pc is what actually named the hang this path had.
static inline void join_phase(const char *what) { bb_note_phase(what); }

#endif  // RPC_RADIO_H
