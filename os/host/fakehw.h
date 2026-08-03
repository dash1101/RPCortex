// A fake device, so package logic can be tested without one.
//
// Packages talk to the world only through fw_* — that was the point of the ABI,
// and it means a host implementation of those calls makes a package's logic
// ordinary testable code. The alternative is what has been happening: writing a
// protocol, flashing it, and finding out from a wrong reading that a threshold
// was off by one.
//
// What this models is the SHAPE of the hardware, not the silicon. A GPIO
// remembers what was written to it, an I2C bus has devices at addresses it was
// told about, and a pin can be given a scripted sequence of levels with
// timestamps so a bit-banged protocol sees exactly the waveform a real part
// would produce. That is enough to test every decision a package makes, which
// is where package bugs live.
//
// It is NOT a claim that the package works on hardware. Timing, electrical
// behaviour and the SDK's own quirks are outside it, and TESTING.md says so.
#ifndef RPC_FAKEHW_H
#define RPC_FAKEHW_H

#include <stdint.h>
#include <stddef.h>

#define FAKE_PINS      32
#define FAKE_EDGES    512
#define FAKE_I2C_DEVS  16
#define FAKE_OUT_MAX  8192

// --- what the test sets up --------------------------------------------------

// Reset everything. Every test starts here so none of them inherit state.
void fake_reset(void);

// A scripted waveform on `pin`: `levels[i]` is held for `hold_us[i]`.
//
// This is how a sensor is simulated. fw_gpio_get walks the script by the fake
// clock, so a package that times pulses sees the durations it was given —
// which is the only way to test a decoder without an oscilloscope.
void fake_pin_script(unsigned pin, const int *levels, const unsigned *hold_us,
                     unsigned n);

// What a package last drove onto a pin, and how many times it changed.
int      fake_pin_level(unsigned pin);
unsigned fake_pin_writes(unsigned pin);

// Put a device on the I2C bus. Reads from it return `resp` cyclically.
void fake_i2c_add(unsigned addr, const unsigned char *resp, unsigned n);

// The raw value fw_adc_read returns for a channel.
void fake_adc_set(unsigned channel, int raw);

// Everything the package printed, so a test can assert on what it said as well
// as on what it did.
const char *fake_output(void);
void        fake_output_clear(void);

// PIO, recorded rather than executed: the words loaded, the divider asked for
// and the values pushed. Enough to check a program is what it should be.
unsigned        fake_pio_program(int h, unsigned short *out, unsigned cap);
unsigned        fake_pio_divider(int h);
unsigned        fake_pio_puts(int h, unsigned long *out, unsigned cap);
void            fake_set_clock_hz(uint32_t hz);

// A fake network. Nothing here reaches a socket; it models what a package can
// observe — connected or not, a list of access points, a name that resolves,
// and one URL that returns a body.
void fake_net_up(const char *ssid, const char *ip);
void fake_net_down(void);
void fake_net_add_ap(const char *ssid, int rssi, int channel, int secured);
void fake_http_serve(const char *url, const char *body);

// The fake clock, in microseconds. Advances on its own during busy waits so a
// package that polls a deadline makes progress.
uint32_t fake_now_us(void);
void     fake_advance_us(uint32_t us);

#endif  // RPC_FAKEHW_H
