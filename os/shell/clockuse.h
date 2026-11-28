// Using something whose timing comes from the system clock.
//
// clk_peri follows clk_sys on these chips, so an SPI, I2C or UART set up at one
// clock runs at the wrong rate after a change, and a change in the middle of a
// transfer corrupts it. The clock governor (governor.cpp) only lowers the clock
// while none of them is in use, and every place that sets one up takes this
// first: under the same lock the governor changes the clock under, and with the
// clock put back to its base before the hardware is touched.
//
//   ClockUse _cu;      // at the top of anything that calls spi_init, i2c_init,
//                      // uart_init, pwm_init or claims a PIO state machine
#ifndef RPC_CLOCKUSE_H
#define RPC_CLOCKUSE_H

#include <stdint.h>

void clock_use_begin(void);
void clock_use_end(void);

struct ClockUse {
    ClockUse()  { clock_use_begin(); }
    ~ClockUse() { clock_use_end(); }
    ClockUse(const ClockUse &) = delete;
    ClockUse &operator=(const ClockUse &) = delete;
};

// The clock peripherals are timed from whenever any is in use, in Hz: the base,
// not wherever the governor has it right now. What fw_clock_hz tells packages,
// since a package computes a divider from it before it claims the hardware.
uint32_t clock_base_hz(void);

// `pulse set`: change the clock and make it the base (governor.cpp).
bool clock_set_base_mhz(unsigned mhz);

// What is powering the board: 1 USB, 0 battery, -1 not known.
int power_source(void);

// For `power status`.
struct GovStatus {
    const char *mode;
    const char *why;
    const char *holder;     // what is holding the clock, or null
    unsigned base_mhz, floor_mhz, cur_mhz;
    unsigned changes;       // how many times it has moved the clock this boot
};
void governor_status(GovStatus *out);
bool governor_set_mode(const char *mode);   // "off" "auto" "on"; saved
void governor_start(void);

#endif
