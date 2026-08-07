#!/usr/bin/env python3
"""Compare the C++ icons against the MicroPython ones they were ported from.

Renders every icon through v1's own novacanvas + novaicons, renders the same
keys through the C++ novasim, and diffs the two pixel for pixel.

This is the check that "ported faithfully" is a fact rather than an intention.
An icon that differs by four pixels is a transcription slip; one that differs by
two hundred is a different drawing, and only one of those is worth a person's
time to look at.

    tools/icondiff.py [path/to/novad1/python/package]
"""
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
V2 = os.path.dirname(HERE)
# A relative default rather than a baked-in path: the two repos sit beside each
# other in every checkout that has both.
DEFAULT_V1 = os.path.join(V2, '..', 'RPCortex-repo', 'repo', 'packages', 'novad1')

W, H, RBIG, RSMALL = 44, 32, 12, 6


def cpp_frames():
    sim = os.path.join(V2, 'os', 'host', 'novasim')
    if not os.path.exists(sim):
        src = os.path.join(V2, 'os', 'host', 'novasim.cpp')
        subprocess.run(['g++', '-std=c++17', '-O2',
                        '-I' + os.path.join(V2, 'os'),
                        '-I' + os.path.join(V2, 'os', 'core'),
                        '-I' + os.path.join(V2, 'os', 'include'),
                        src, '-o', sim], check=True)
    out = subprocess.run([sim, '--dump'], capture_output=True, text=True, check=True)
    frames = {}
    for line in out.stdout.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            frames[parts[0]] = parts[3]
    return frames


def py_frames(v1dir, keys):
    sys.path.insert(0, v1dir)
    import novacanvas
    import novaicons
    frames = {}
    for k in keys:
        c = novacanvas.Canvas(W, H)
        try:
            novaicons.draw(c, k, 15, 16, RBIG)
            novaicons.draw(c, k, 36, 16, RSMALL)
        except Exception as e:
            frames[k] = None
            continue
        bits = []
        for y in range(H):
            for x in range(W):
                byte = c.buf[(y >> 3) * W + x]
                bits.append('1' if (byte >> (y & 7)) & 1 else '0')
        frames[k] = ''.join(bits)
    return frames


def main():
    v1 = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_V1
    if not os.path.isdir(v1):
        sys.exit('no MicroPython suite at %s' % v1)

    cpp = cpp_frames()
    py = py_frames(v1, list(cpp.keys()))

    same = near = differ = missing = 0
    rows = []
    for k in cpp:
        a, b = cpp[k], py.get(k)
        if b is None:
            missing += 1
            rows.append((999999, k, 'not in the MicroPython set'))
            continue
        d = sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])
        lit = max(a.count('1'), b.count('1'))
        if d == 0:
            same += 1
        elif lit and d * 100 // lit <= 10:
            near += 1
            rows.append((d, k, '%d px (%d%% of the drawing)' % (d, d * 100 // lit)))
        else:
            differ += 1
            rows.append((d, k, '%d px (%d%% of the drawing)' % (d, d * 100 // max(lit, 1))))

    rows.sort(reverse=True)
    for _, k, why in rows:
        print('  %-14s %s' % (k, why))
    print()
    print('  %d identical, %d within 10%%, %d different, %d absent upstream'
          % (same, near, differ, missing))
    # Not a pass/fail: an icon can legitimately differ where the C++ one was
    # improved on purpose, and the point of this tool is to make every one of
    # those a decision somebody made rather than a drift nobody saw.
    return 0


if __name__ == '__main__':
    sys.exit(main())
