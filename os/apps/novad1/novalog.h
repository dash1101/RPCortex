// Desc: A capped event log on flash.
// File: novalog.h
//
// EVENTS ONLY, never per-frame. Each flash write costs around ten milliseconds
// and littlefs is a wear-levelling filesystem on a part with a finite number of
// erase cycles — a log that recorded every redraw would be both a stutter and a
// way to wear the flash out.
//
// Capped by REWRITING rather than appending forever, so a device left running
// for a month has a log somebody can read and not a file that fills the disk.
#ifndef NOVA_LOG_H
#define NOVA_LOG_H

namespace nova {
namespace log {

// The last N lines are kept. Sixty is what fits on a few screens and is enough
// to see what led up to something.
constexpr int MAX_LINES = 60;

// Write one line, stamped with the time when the clock is set.
void write(const char *msg);

// Read a line back, newest first. Returns false past the end.
bool line(int i, char *out, unsigned cap);
int  count(void);

void clear(void);

}  // namespace log
}  // namespace nova

#endif  // NOVA_LOG_H
