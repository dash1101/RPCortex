// Whether a sleep request is one the device can come back from.
//
// Pure, and separate from shell/power.cpp for the same reason preempt.cpp is
// separate from the scheduler: the decision is the risky part, the hardware is
// not, and a decision with no hardware in it can be tested properly.
//
// The rules are less obvious than they look. Zero milliseconds means "no
// deadline", which is fine with a wake pin and fatal without one. The minimum
// differs by two orders of magnitude between the chips. And a pin does not
// excuse a duration below that minimum, because the timer is what enforces it.
#ifndef RPC_POWERPOLICY_H
#define RPC_POWERPOLICY_H

enum PowerCheck {
    POWER_OK = 0,
    POWER_TOO_SHORT,        // below what the hardware will honour
    POWER_NO_WAKE,          // no duration and no pin: nothing would wake it
    POWER_BAD_PIN,
};

PowerCheck  power_check(unsigned ms, int wake_pin, unsigned pin_count, unsigned min_ms);
const char *power_check_str(PowerCheck c);

#endif  // RPC_POWERPOLICY_H
