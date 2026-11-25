#!/usr/bin/env python3
"""The package ABI table only grows at the end. See os/abi_symbols.txt.

    python3 tools/check-abi-order.py [repo root]
"""
import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
src = open(os.path.join(root, "os", "api.cpp")).read()
start = src.index("kSymbols[]")
table = re.findall(r"SYM\(([A-Za-z0-9_]+)\)", src[start:src.index("};", start)])
frozen = [l.strip() for l in open(os.path.join(root, "os", "abi_symbols.txt"))
          if l.strip() and not l.startswith("#")]

for i, (have, want) in enumerate(zip(table, frozen)):
    if have != want:
        print("os/api.cpp's symbol table changed at index {}: {} where {} was.".format(i, have, want))
        print("Every package built since uses these indices; a new symbol goes at the END.")
        sys.exit(1)
if len(table) < len(frozen):
    print("os/api.cpp's symbol table lost {} from the end ({} ...).".format(
        len(frozen) - len(table), frozen[len(table)]))
    sys.exit(1)
if len(table) > len(frozen):
    print("New symbols at the end of the table: {}.".format(", ".join(table[len(frozen):])))
    print("Append them to os/abi_symbols.txt too, in the same order.")
    sys.exit(1)
print("{} symbols, in the order packages were built against".format(len(table)))
