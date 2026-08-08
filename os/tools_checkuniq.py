#!/usr/bin/env python3
"""Refuse two package files that share a file-scope name.

Every screen file uses `static` for its own internals, which is right on the
device: separate translation units, no clash. But novagui_test compiles the
whole package into ONE unit so it can drive the runner, and there two files
with a `g_out`, an `OUT_MAX`, a `shell_task` or a `DeviceScreen` are a
redefinition.

That is a build failure in a harness nobody runs until later, produced by a
change that looked completely fine — and it has now happened three times, with
four different names. Cheaper to refuse it here, naming both files, than to
read it out of a template-instantiation error half an hour afterwards.
"""
import collections, glob, os, re, sys

FILES = sorted(glob.glob(os.path.join(os.path.dirname(__file__), 'apps/novad1/*.cpp')))

PATTERNS = [
    re.compile(r'^(?:class|struct)\s+(\w+)\s*(?::|\{)', re.M),      # types
    re.compile(r'^enum\s+(?:class\s+)?(\w+)\s*(?::|\{)', re.M),      # and enums
    re.compile(r'^static\s+[\w:<>*&\s]+?\b(\w+)\s*\(', re.M),        # functions
    re.compile(r'^static\s+(?:const\s+)?[\w:<>*&\s]+?\**(\w+)\s*(?:\[|=|;)', re.M),
    re.compile(r'^\s*constexpr\s+\w[\w:]*\s+(\w+)\s*=', re.M),       # constants
    re.compile(r'^#define\s+([A-Z][A-Z0-9_]{2,})', re.M),
]

ENUMERATORS = re.compile(r'^enum\s+(?:class\s+)?\w*\s*(?::[^{]*)?\{([^}]*)\}', re.M | re.S)

owner = collections.defaultdict(set)
for path in FILES:
    text = open(path).read()
    name = os.path.basename(path)
    for pat in PATTERNS:
        for m in pat.finditer(text):
            owner[m.group(1)].add(name)
    # An UNSCOPED enum puts its enumerators in the enclosing scope, so two
    # files with a PEND_NONE collide exactly as two with a g_out do.
    for m in ENUMERATORS.finditer(text):
        if m.group(0).startswith('enum class'):
            continue
        for tok in m.group(1).split(','):
            tok = tok.split('=')[0].strip()
            if re.fullmatch(r'[A-Za-z_]\w*', tok or ''):
                owner[tok].add(name)

clashes = {k: sorted(v) for k, v in owner.items() if len(v) > 1}
if not clashes:
    sys.exit(0)

print('Two package files share a file-scope name, which the single-unit host')
print('harness cannot compile. Give one of each pair a prefix:')
for k in sorted(clashes):
    print('    {:<20} {}'.format(k, ', '.join(clashes[k])))
sys.exit(1)
