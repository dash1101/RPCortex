// Small formatting helpers shared by the listing commands.
//
// Pure, so the rounding boundaries are pinned by a host test rather than
// discovered when a file lands on the wrong side of one.
#ifndef RPC_FMT_H
#define RPC_FMT_H

#include <stdint.h>

// Human-readable byte count, v1's _fmt_size exactly: "512B", "1.5K", "12K",
// "1.2M", "34M". Below 10 in a unit keeps one decimal; at or above 10 it drops
// to a whole number, which is what keeps the column narrow.
void fmt_size(uint32_t bytes, char *out, uint32_t cap);

// "YYYY-MM-DD HH:MM:SS" from a Unix epoch. Writes "-" when `epoch` is 0, which
// is how an unknown timestamp is reported — a date of 1970 looks like corruption
// where a dash reads as "not recorded".
void fmt_time(uint32_t epoch, char *out, uint32_t cap);

#endif  // RPC_FMT_H
