#!/usr/bin/env python3
"""Verify every stack switch lets go of the stack limit first.

On ARMv8-M, writing a value below MSPLIM into SP is itself a stack overflow —
the check is on the write, not on some later push. Every task's stack comes from
the heap in main SRAM while the boot stack sits high in SCRATCH_Y, so a context
switch out of the boot context moves SP tens of kilobytes below the limit that
was protecting it.

That is not a subtle failure. It hard-faults on the `mov sp, rN` itself, on the
very first switch into the shell task, and the device boot-loops with no shell
to report from — which is exactly what happened. It also cannot be caught on the
host: there is no MSPLIM there, and every scheduler test passes.

So the built image is checked instead. Each routine below must contain
`msr MSPLIM` before it writes SP, and the two must be close enough together that
nothing has been slipped between them.

RP2040 is skipped: ARMv6-M has no MSPLIM and no such rule.
"""
import re
import subprocess
import sys

# Routine -> how many instructions may sit between the release and the SP write.
# Small on purpose: the point is that they are adjacent, not merely both present.
ROUTINES = {
    'task_ctx_switch':      3,
    'app_call_unpriv':      3,
    'app_call_unpriv_tail': 3,
}


def disassemble(elf):
    out = subprocess.run(['arm-none-eabi-objdump', '-d', elf],
                         capture_output=True, text=True, check=True).stdout
    bodies, name = {}, None
    for line in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if m:
            name = m.group(1)
            bodies[name] = []
        elif name and re.match(r'^\s*[0-9a-f]+:', line):
            bodies[name].append(line)
    return bodies


def main():
    if len(sys.argv) < 2:
        print('usage: check-stackswitch.py <elf>', file=sys.stderr)
        return 2
    elf = sys.argv[1]
    bodies = disassemble(elf)

    # No MSPLIM anywhere means ARMv6-M, where none of this applies.
    if not any('MSPLIM' in l for body in bodies.values() for l in body):
        print('  stackswitch     ok   ARMv6-M: no stack limit register')
        return 0

    bad = 0
    checked = 0
    for routine, slack in ROUTINES.items():
        body = bodies.get(routine)
        if body is None:
            continue                      # not in this build
        writes = [i for i, l in enumerate(body)
                  if re.search(r'\bmov\s+sp,\s*r', l)]
        if not writes:
            continue                      # nothing to guard
        for w in writes:
            checked += 1
            window = body[max(0, w - slack):w]
            if not any('MSPLIM' in l for l in window):
                print('  FAIL %s writes SP without releasing the stack limit first:'
                      % routine)
                for l in body[max(0, w - slack):w + 1]:
                    print('       ' + l.strip())
                bad += 1

    if not checked:
        print('  FAIL no stack switches found at all - nothing was checked')
        return 1

    # And the other half of the same rule, for privilege rather than for SP.
    #
    # The instruction after `msr CONTROL` runs under the NEW privilege, and
    # everything in flash is unreachable once that is unprivileged — so a
    # routine in flash that drops privilege and then branches faults on the
    # branch, after the pipeline flush forces it to be re-fetched. It is not the
    # branch that is wrong, it is where the branch lives.
    #
    # Both routines below therefore hand the value to a gate in the package's
    # own veneer pool and jump there while still privileged. Neither may contain
    # a CONTROL write at all. sandbox_svc is exempt and is not listed: it runs in
    # handler mode and only ever RAISES privilege, which needs no gate.
    for routine in ('app_call_unpriv', 'sandbox_syscall_return'):
        body = bodies.get(routine)
        if body is None:
            continue
        for i, l in enumerate(body):
            if re.search(r'\bmsr\s+CONTROL', l):
                print('  FAIL %s writes CONTROL in flash; the drop belongs in '
                      'the veneer pool gate:' % routine)
                print('       ' + l.strip())
                bad += 1
        checked += 1

    if bad:
        return 1
    print('  stackswitch     ok   %d switch(es) release MSPLIM, and privilege '
          'is dropped only in the gate' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
