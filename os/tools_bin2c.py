#!/usr/bin/env python3
"""Turn a built .app into a C array so the firmware can carry it.

Used for the packages that ship installed: the bytes live in flash, and the OS
writes them into /pkg on a first boot. That is what makes them real packages —
removable like any other — rather than built-in commands pretending to be.
"""
import sys, os
src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('// Generated from %s. Do not edit.\n' % os.path.basename(src))
    f.write('#include <stdint.h>\n\n')
    f.write('extern "C" const unsigned char %s_data[] = {\n' % name)
    for i in range(0, len(data), 16):
        f.write('    ' + ''.join('0x%02x, ' % b for b in data[i:i+16]).rstrip() + '\n')
    f.write('};\n')
    f.write('extern "C" const unsigned int %s_len = %d;\n' % (name, len(data)))
print('%s -> %s (%d bytes)' % (os.path.basename(src), os.path.basename(dst), len(data)))
