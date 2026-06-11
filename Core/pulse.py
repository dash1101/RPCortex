# Desc: Hardware management and clock tuning for RPCortex - Nebula OS
# File: /Core/pulse.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2
# Author: dash1101

import gc, sys, machine
from Core.RPCortex import multi, fatal, error, info, warn, ok, inpt

def mem_check():
    """
    Fast RAM sanity check — allocates one buffer, writes a stride pattern,
    verifies it, then releases. Completes in milliseconds on any platform.

    Test size: 10% of free RAM, capped at 64 KB, minimum 4 KB.
    8 evenly-spaced bytes are written and verified — enough to catch stuck
    bits or address aliasing without exhausting the heap.
    """
    try:
        gc.collect()
        free_memory = gc.mem_free()
        ok("Available memory: {:.1f} KB".format(free_memory / 1024))

        if free_memory < 65536:
            warn("Less than 64 KB free. Performance may suffer!")

        test_size = min(free_memory // 10, 65536)
        if test_size < 4096:
            test_size = 4096

        try:
            buf = bytearray(test_size)
        except MemoryError:
            fatal("Cannot allocate {:.1f} KB for memory test!".format(test_size / 1024))
            return False

        # Write a stride pattern across 8 evenly-spaced positions
        stride = test_size // 8
        for i in range(8):
            buf[i * stride] = (i * 37) & 0xFF

        # Verify the pattern was retained
        for i in range(8):
            if buf[i * stride] != (i * 37) & 0xFF:
                del buf
                fatal("Memory verification failed at byte {}.".format(i * stride))
                return False

        del buf
        gc.collect()
        ok("Memory OK — {:.1f} KB tested, {:.1f} KB free.".format(
            test_size / 1024, gc.mem_free() / 1024))
        return True

    except Exception as err:
        fatal("Memory check failed: {}".format(err))
        return False

def cpu_check():
    if machine.freq() < 100000000:
        warn("Processor frequency is low, performance may suffer.")
    else:
        ok("Processor frequency is sufficient.")
    try:
        float_result = (100.0 * 10.0) / 2.0 + 50.0 - 25.0
        if float_result == 525.0:
            ok("Floating-point operations passed.")
        else:
            fatal("Floating-point operations failed. Expected 525.0, got {}.".format(float_result))
            return False
        int_result = (100 * 10) // 2 + 50 - 25
        if int_result == 525:
            ok("Integer operations passed.")
        else:
            fatal("Integer operations failed. Expected 525, got {}.".format(int_result))
            return False
        if 100 < 200 and 500 > 250:
            ok("Comparison operations passed.")
        else:
            fatal("Comparison operations failed.")
            return False
        bitwise_result = (0b1100 & 0b1010) == 0b1000
        if bitwise_result:
            ok("Bitwise operations passed.")
        else:
            fatal("Bitwise operations failed.")
            return False
        ok("CPU checks passed successfully.")
        return True

    except Exception as e:
        fatal("CPU check encountered an error: {}".format(e))
        return False

def set_clock(target_mhz, verbose=True):
    """
    Set CPU clock to target_mhz MHz.
    Returns a string like '220.0MHz' representing the result.
    """
    try:
        machine.freq(int(target_mhz) * 1_000_000)
        if verbose:
            ok("CPU set to {} MHz".format(target_mhz))
        return "{:.1f}MHz".format(float(target_mhz))
    except Exception as e:
        if verbose:
            error("Failed to set {} MHz: {}".format(target_mhz, e))
        return "{:.1f}MHz".format(machine.freq() / 1_000_000)

# Backward-compat alias — post.py uses pulse.overclock() in check_oc()
overclock = set_clock

# The benchmark moved out of pulse.py into a package — now /Packages/PulseMark/
# pulsemark.py (the `bench` command; renamed from NebulaMark in v0.9.1) so it can
# be updated via the package manager.
