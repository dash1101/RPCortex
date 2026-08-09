#!/usr/bin/env python3
"""Verify a package only needs what the firmware gives it.

Two checks, both of which otherwise fail on the device and not here.

SYMBOLS. An unresolved symbol is only discovered when the package is LOADED,
which means on the device, at the moment someone tries to use it — and the
message ("unresolved symbol - __aeabi_idivmod") names a function the author never
wrote and has no reason to recognise. So: nm the package, read the exported table
out of api.cpp, and fail the build on anything missing.

STATIC CONSTRUCTORS. The loader does not run .init_array. A package holding a
global whose type has a constructor gets no error of any kind — the object is
simply left as zeroes and behaves like one that was never set up, which surfaces
later as a field with the wrong value and no way to see why. Any .init_array at
all is refused here instead.
"""
import re, subprocess, sys, os

app, api_cpp, nm = sys.argv[1], sys.argv[2], sys.argv[3]
name = os.path.basename(app)

# --- static constructors ----------------------------------------------------
#
# readelf comes from the same toolchain as nm, so deriving its path rather than
# taking a fourth argument keeps the call sites unchanged.
readelf = re.sub(r'nm$', 'readelf', nm)
sections = subprocess.run([readelf, '-S', app], capture_output=True, text=True)
if sections.returncode == 0:
    # --- how many sections ---------------------------------------------------
    #
    # The loader reads the section table into a fixed array, and past that it
    # refuses the package with "too many sections" — on the device, at install
    # time, naming nothing that appears in the source. C++ is what gets a package
    # here: every inline function, vtable and template instantiation can arrive
    # as its own section plus a .group describing it, so section count tracks the
    # number of CLASSES rather than the amount of code.
    #
    # Keep in step with LOADER_MAX_SECTIONS in loader-spike/firmware/loader.h.
    LOADER_MAX_SECTIONS = 128
    n_sections = len(re.findall(r'^\s*\[\s*\d+\]', sections.stdout, re.M))
    if n_sections > LOADER_MAX_SECTIONS:
        print('\n%s has %d ELF sections; the loader accepts %d.'
              % (name, n_sections, LOADER_MAX_SECTIONS), file=sys.stderr)
        print('\nA multi-file package should be partially linked through\n'
              'os/apps_partial.ld, which folds everything into four sections and\n'
              'stays there however many classes it gains. A single-file package\n'
              'that has got here has enough C++ in it to want the same treatment.\n',
              file=sys.stderr)
        sys.exit(1)

    # --- static constructors -------------------------------------------------
    ctors = []
    for line in sections.stdout.splitlines():
        m = re.search(r'\.(init_array|ctors|preinit_array)\b', line)
        if m:
            ctors.append(m.group(0))
    if ctors:
        print('\n%s has %s, which the loader does not run.' % (name, ' and '.join(sorted(set(ctors)))),
              file=sys.stderr)
        print('\nSomething in this package is a global whose type has a constructor. It\n'
              'would never be constructed on the device, and there would be no error —\n'
              'just an object full of zeroes behaving as though it had been set up.\n'
              '\n'
              'Usual causes, in the order they usually turn out to be:\n'
              '    a static/global class object with default member initialisers\n'
              '    a function-local static of a class type (also wants a guard symbol)\n'
              '    a global array of a type with a constructor\n'
              '\n'
              'Make the type trivially constructible and initialise it in an init()\n'
              'the package calls itself. bss starts as zeroes, which is usually what\n'
              'the initialisers were setting anyway.\n', file=sys.stderr)
        sys.exit(1)

# --- position-independent invariant -----------------------------------------
#
# A package built -fPIC -msingle-pic-base reaches its data through a GOT off r9,
# so the half that goes to a flash slot — every allocatable section that is NOT
# writable: .text and .rodata — must carry NO absolute address. Only GOT_BREL
# (data via the GOT) and THM_CALL / THM_JUMP24 (firmware, handled by veneers) may
# appear, and those bake to a GOT offset or a blob-relative branch, both of which
# are the same wherever the slot lands. An ABS32 that slips in — a function
# pointer the compiler failed to route through the GOT, a const table of pointers
# left in .rodata — links and loads and then holds a stale address the moment the
# blob runs from flash. The loader would refuse it (LOAD_ERR_RELOC_UNSUPPORTED),
# but by then it is a device that will not take an update; refused here instead,
# at build time, where it names the section and the type.
#
# The whole read-only half is checked, not only .text: the "assemble once, place
# anywhere" property is what stage 3 rests on, and .rodata being clean is now
# load-bearing rather than incidental. Only a PIC package (one with any GOT_BREL)
# is held to this; a plain package relocates into RAM and is unaffected.
rel = subprocess.run([readelf, '-r', app], capture_output=True, text=True)
if rel.returncode == 0 and 'R_ARM_GOT_BREL' in rel.stdout:
    ALLOWED = {'R_ARM_GOT_BREL', 'R_ARM_THM_CALL', 'R_ARM_THM_JUMP24', 'R_ARM_NONE'}

    # Section flags, to tell a slot-resident section (Alloc, not Write) from a
    # writable one whose ABS32 fixups are applied in RAM at load and are fine.
    secflags = {}
    shdr = subprocess.run([readelf, '-SW', app], capture_output=True, text=True)
    for line in shdr.stdout.splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\.\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+'
                     r'\s+[0-9a-f]+\s+[0-9a-f]+\s+(\S*)', line)
        if m:
            secflags[m.group(1)] = m.group(2)

    ro_now = False          # are we inside a relocation section targeting the slot?
    ro_name = ''
    offenders = {}          # (section, type) -> count
    for line in rel.stdout.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            tgt = re.sub(r'^\.rela?', '', m.group(1))   # .rel.text -> .text
            fl = secflags.get(tgt, '')
            ro_now = ('A' in fl and 'W' not in fl)      # allocatable, read-only
            ro_name = tgt
            continue
        if ro_now:
            mm = re.search(r'\b(R_ARM_\w+)\b', line)
            if mm and mm.group(1) not in ALLOWED:
                offenders[(ro_name, mm.group(1))] = offenders.get((ro_name, mm.group(1)), 0) + 1
    if offenders:
        print('\n%s is position-independent but its read-only half carries %d '
              'absolute relocation(s):' % (name, sum(offenders.values())), file=sys.stderr)
        for (sec, t), c in sorted(offenders.items()):
            print('    %s in %s x%d' % (t, sec, c), file=sys.stderr)
        print('\nA PIC package must reach every global through the GOT. An absolute\n'
              'relocation in a slot-resident section is an address that would be wrong\n'
              'the moment the blob runs from flash — a function pointer the compiler\n'
              'could not route through r9, or a const pointer table the linker left in\n'
              '.rodata. Check it is built with\n'
              '    -fPIC -msingle-pic-base -mno-pic-data-is-text-relative\n', file=sys.stderr)
        sys.exit(1)

# --- symbols ----------------------------------------------------------------

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
