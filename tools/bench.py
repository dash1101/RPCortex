# RPCMark for RPCortex v1.0 (MicroPython) — the other half of the comparison.
#
# The workloads and iteration counts match os/apps/bench/bench.cpp exactly, so
# the two scores mean the same thing. Anything that differed between them would
# make the comparison a story rather than a measurement.
#
# Copy to a v1 device and run:  exec bench.py
#
# No f-strings and no keyword arguments to str.split — the house rules for
# MicroPython here, and this has to run on the real thing.

import utime

INT_ITER   = 200000
MEM_BYTES  = 8192
MEM_PASSES = 40
CALL_ITER  = 100000
STR_ITER   = 2000
FS_ITER    = 20

# The same reference figures the C++ side uses, so both print comparable scores.
REF = {
    'integer': 18700,
    'memory':   9400,
    'function': 8100,
    'string':   6200,
    'filesys':   420,
}


def bench_int():
    t0 = utime.ticks_ms()
    acc = 0
    for i in range(1, INT_ITER + 1):
        acc += i
        acc ^= (acc >> 3)
        acc += (i * 7)
        acc &= 0xFFFFFFFF          # C++ wraps at 32 bits; match it
    return utime.ticks_diff(utime.ticks_ms(), t0)


def bench_mem():
    buf = bytearray(MEM_BYTES)
    for i in range(MEM_BYTES):
        buf[i] = i & 0xFF
    t0 = utime.ticks_ms()
    total = 0
    for _ in range(MEM_PASSES):
        for i in range(MEM_BYTES):
            total += buf[i]
    return utime.ticks_diff(utime.ticks_ms(), t0)


def _leaf(a, b):
    return a + b


def bench_call():
    t0 = utime.ticks_ms()
    acc = 0
    for i in range(CALL_ITER):
        acc = _leaf(acc, i)
    return utime.ticks_diff(utime.ticks_ms(), t0)


def bench_str():
    t0 = utime.ticks_ms()
    hits = 0
    for i in range(STR_ITER):
        s = "item" + str(i)
        for ch in s:
            if ch == '7':
                hits += 1
    return utime.ticks_diff(utime.ticks_ms(), t0)


def bench_fs():
    data = b'abcdefghijklmnopqrstuvwxyz' * 10
    data = data[:256]
    t0 = utime.ticks_ms()
    for _ in range(FS_ITER):
        try:
            with open('/tmp_bench.tmp', 'wb') as f:
                f.write(data)
            with open('/tmp_bench.tmp', 'rb') as f:
                f.read()
            import uos
            uos.remove('/tmp_bench.tmp')
        except OSError:
            break
    return utime.ticks_diff(utime.ticks_ms(), t0)


def _score(ms, ref):
    if ms <= 0:
        ms = 1
    return (ref * 200) // ms


def main():
    print("")
    print("=== RPCMark (MicroPython) ===")
    print("Same workload as the C++ build, so the numbers compare.")
    print("")
    print("  {:<12} {:>9}   {:>5}".format("TEST", "TIME", "SCORE"))
    print("  " + "-" * 30)

    total = 0
    for name, fn in (('integer',  bench_int),
                     ('memory',   bench_mem),
                     ('function', bench_call),
                     ('string',   bench_str),
                     ('filesys',  bench_fs)):
        ms = fn()
        sc = _score(ms, REF[name])
        total += sc
        print("  {:<12} {:>6} ms   {:>5}".format(name, ms, sc))

    print("  " + "-" * 30)
    print("  {:<12} {:>9}   {:>5}".format("TOTAL", "", total))
    print("")
    print("  Run 'bench' on a v2 device for its number.")
    print("")


main()
