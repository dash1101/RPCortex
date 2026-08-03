#!/usr/bin/env python3
"""Verify every symbol a package needs is one the firmware exports.

An unresolved symbol is only discovered when the package is LOADED, which means
on the device, at the moment someone tries to use it — and the message
("unresolved symbol - __aeabi_idivmod") names a function the author never wrote
and has no reason to recognise. That is a terrible place to find out.

This runs at build time instead: nm the package, read the exported table out of
api.cpp, and fail the build on anything missing.
"""
import re, subprocess, sys, os

app, api_cpp, nm = sys.argv[1], sys.argv[2], sys.argv[3]

out = subprocess.run([nm, '-u', app], capture_output=True, text=True)
if out.returncode != 0:
    sys.exit('could not read %s: %s' % (app, out.stderr.strip()))
needed = [l.split()[-1] for l in out.stdout.splitlines() if l.strip().startswith('U')]

src = open(api_cpp).read()
# The table is a list of SYM(name) entries; anything commented out does not count.
exported = set()
for line in src.splitlines():
    if line.strip().startswith('//'):
        continue
    exported.update(re.findall(r'\bSYM\(([A-Za-z_][A-Za-z0-9_]*)\)', line))

missing = [s for s in needed if s not in exported]
name = os.path.basename(app)
if missing:
    print('\n%s needs %d symbol(s) the firmware does not export:' % (name, len(missing)),
          file=sys.stderr)
    for m in missing:
        print('    %s' % m, file=sys.stderr)
    print('\nAdd them to the table in os/api.cpp. If they start with __aeabi_ they are\n'
          'compiler helpers emitted for arithmetic the source never mentions.\n',
          file=sys.stderr)
    sys.exit(1)

print('%s: %d symbol(s), all resolved' % (name, len(needed)))
