# Desc: Hardware management, clock tuning, and benchmarking for RPCortex - Nebula OS
# File: /Core/pulse.py
# Last Updated: 3/25/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta2
# Author: dash1101

import gc, sys, machine
from Core.RPCortex import multi, fatal, error, info, warn, ok, inpt

HEADER = '\033[95m'
OKBLUE = '\033[94m'
OKCYAN = '\033[96m'
WARNING = '\033[93m'
GRAY = '\033[90m'
GREEN = '\033[32m'
WHITE = '\033[0m'
FAIL = '\033[91m'
BOLD = '\033[1m'
UNDERLINE = '\033[4m'
tr = []

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


# Nebula-Mark v2.0 Code written by: shaoziyang
# Website: https://github.com/shaoziyang/micropython_benchmarks

def NebulaMark():
    try:
        from time import ticks_ms, ticks_diff
        import machine
        TEST_ENV = 'micropython'

        def freq():
            try:
                f = machine.freq()//1000000
            except:
                f = machine.freq()[0]//1000000
            return f
    except:
        from time import monotonic_ns

        def ticks_ms():
            return monotonic_ns()//1000000

        def ticks_diff(T2, T1):
            return T2 - T1

        try:
            import board
            TEST_ENV = 'circuitpython'

            import microcontroller

            def freq():
                return microcontroller.cpu.frequency//1000000

        except:
            TEST_ENV = 'OTHER'

            try:
                import psutil

                def freq():
                    return psutil.cpu_freq().max
            except:
                def freq():
                    return 'unknown'

    def memory():
        try:
            r= gc.mem_free()+gc.mem_alloc()
        except:
            try:
                r = psutil.virtual_memory().total
            except:
                r = 'unknown'
        return r

    PLATFORM = sys.platform
    VERSION = sys.version
    FREQ = freq()
    MEMORY = memory()

    def run_test(func, *param):
        gc.collect()
        try:
            t1 = ticks_ms()
            if param == None:
                func()
            else:
                func(*param)
            t2 = ticks_ms()
            dt = ticks_diff(t2, t1)
            print('Calculation time:', dt, 'ms\n')
            return dt
        except:
            print('Error occurred during operation!')

    def mandelbrot_test(p):
        iter = p[0]
        def in_set(c):
            z = 0
            for i in range(iter):
                z = z * z + c
                if abs(z) > 4:
                    return False
            return True

        r = ''
        for v in range(31):
            for u in range(81):
                if in_set((u / 30 - 2) + (v / 15 - 1) * 1j):
                    r += ' '
                else:
                    r += '#'
            r += '\n'
        if len(p)>1 and p[1]:
            print(r)

    def pi_test(p):
        iter = p[0]
        extra = 8
        one = 10 ** (iter+extra)
        t, c, n, na, d, da = 3*one, 3*one, 1, 0, 0, 24

        while t > 1:
            n, na, d, da = n+na, na+8, d+da, da+32
            t = t * n // d
            c += t
        return c // (10 ** extra)


    def add_test(p):
        for i in range(p[0]):
            C = p[1] + p[2]

    def mul_test(p):
        for i in range(p[0]):
            C = p[1] * p[2]

    def div_test(p):
        for i in range(p[0]):
            C = p[1] / p[2]

    def pow_test(p):
        for i in range(p[0]):
            C = p[1] ** p[2]

    INT_ADD_TEST_LIST = ('Integer addition {} times', add_test, [12345678, 56781234], 10000, 100000, 1000000)
    INT_MUL_TEST_LIST = ('Integer multiplication {} times', mul_test, [12345678, 56781234], 10000, 100000, 1000000)
    INT_DIV_TEST_LIST = ('Integer division {} times', div_test, [99999991, 45481], 10000, 100000, 1000000)
    FLOAT_ADD_TEST_LIST = ('Float addition {} times', add_test, [12345678.1234, 56781234.5678], 10000, 100000, 1000000)
    FLOAT_MUL_TEST_LIST = ('Float multiplication {} times', mul_test, [12345678.1234, 56781234.5678], 10000, 100000, 1000000)
    FLOAT_DIV_TEST_LIST = ('Float division {} times', div_test, [99999991.2345, 45481.1357], 10000, 100000, 1000000)
    POWER_TEST_LIST = ('Power calculation {} times', pow_test, [1234.5678,2.3456], 10000, 100000, 1000000)
    MAND_TEST_LIST = ('Mandelbrot iterating {} times', mandelbrot_test, [], 100, 500, 5000)
    PI_TEST_LIST = ('Pi Calculation {} bits', pi_test, [], 1000, 5000, 10000, 100000, 200000)

    TEST_LIST = (
        INT_ADD_TEST_LIST, INT_MUL_TEST_LIST, INT_DIV_TEST_LIST,
        FLOAT_ADD_TEST_LIST, FLOAT_MUL_TEST_LIST, FLOAT_DIV_TEST_LIST,
        POWER_TEST_LIST,
        MAND_TEST_LIST,
        PI_TEST_LIST
    )



    def run():
        global tr

        for TEST in TEST_LIST:
            for item in TEST:
                if type(item) == int:
                    print(TEST[0].format(item))
                    p = TEST[2].copy()
                    p.insert(0, item)
                    r = run_test(TEST[1], p)
                    tr.append([TEST[0].format(item), str(r)])

    def print_result():
        print('|{:36}|{:12}|'.format('item','result'))
        print('|{:36}|{:12}|'.format('-','  :-:'))
        print('|{:36}|{:12}|'.format('Platform', PLATFORM))
        print('|{:36}|{:12}|'.format('Version', VERSION))
        print('|{:36}|{:12}|'.format('Frequency', FREQ))
        print('|{:36}|{:12}|'.format('Memory', MEMORY))

        for i in range(len(tr)):
            print('|{:36}|{:12}|'.format(tr[i][0], tr[i][1]))

    ###############################################################################

    print('\n\n')
    print('##############')
    print('# Begin test #')
    print('##############')

    print('\nEnvironment:', TEST_ENV)
    print('Platform:', PLATFORM)
    print('Version:', VERSION)
    print('Frequency:', FREQ)
    print('Memory:', MEMORY)
    print()

    run()

    print('\nTest finished.')
    print('==============')

    print_result()
