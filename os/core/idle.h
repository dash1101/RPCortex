// Idle auto-logout: the arithmetic, kept apart from the shell that uses it.
//
// v1 logged a session out after `Settings.Idle_Logout` minutes without a
// keystroke. The shell reads it again at every prompt, so changing the setting
// takes effect on the next command rather than the next login. The decision is
// a few lines of unsigned arithmetic, and the one way to get it wrong is the
// millisecond counter wrapping (every 49.7 days), so it lives here where a test
// can drive it across the wrap.
#ifndef RPC_IDLE_H
#define RPC_IDLE_H

#include <stdint.h>

// The longest setting accepted, in minutes. A day: anything longer is a typo for
// "off" or a unit mix-up, and an enormous value would overflow the millisecond
// arithmetic below.
#define IDLE_MAX_MINUTES 1440

// Turn the registry text into milliseconds. Whole minutes only. Empty, zero,
// negative, non-numeric or trailing-garbage values all mean OFF (0), which is the
// safe reading: a setting that cannot be understood must not start logging
// people out. Values above IDLE_MAX_MINUTES are clamped to it.
uint32_t idle_timeout_ms(const char *minutes_text);

// Has `limit_ms` passed since `last_input_ms`? Wrap-safe: both are readings of
// the same free-running 32-bit counter, so the elapsed time is their unsigned
// difference. A `limit_ms` of 0 never expires.
bool idle_expired(uint32_t last_input_ms, uint32_t now_ms, uint32_t limit_ms);

#endif
