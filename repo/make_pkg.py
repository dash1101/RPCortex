#!/usr/bin/env python3
"""
make_pkg.py — Build a .pkg archive for RPCortex from a package source directory.

Usage:
  python make_pkg.py <source_dir>
  python make_pkg.py <source_dir> -o mypackage.pkg

The source directory must contain a package.cfg file.
Output is a standard ZIP file renamed to .pkg.

Run this on your PC (CPython), not on the Pico.
"""

import argparse
import os
import sys
import zipfile


def make_pkg(source_dir, output_path=None):
    source_dir = os.path.abspath(source_dir.rstrip('/\\'))

    if not os.path.isdir(source_dir):
        print(f"Error: '{source_dir}' is not a directory.", file=sys.stderr)
        sys.exit(1)

    cfg_path = os.path.join(source_dir, 'package.cfg')
    if not os.path.isfile(cfg_path):
        print(f"Error: '{cfg_path}' not found. A package.cfg is required.", file=sys.stderr)
        sys.exit(1)

    pkg_name = os.path.basename(source_dir)

    if output_path is None:
        output_path = pkg_name.lower() + '.pkg'

    print(f"Packaging '{source_dir}' -> '{output_path}'")

    with zipfile.ZipFile(output_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(source_dir):
            # Sort for deterministic output
            dirs.sort()
            for filename in sorted(files):
                full_path = os.path.join(root, filename)
                # Archive path: <pkg_name>/<relative_path>
                rel = os.path.relpath(full_path, os.path.dirname(source_dir))
                arc = rel.replace('\\', '/')
                zf.write(full_path, arc)
                print(f"  + {arc}")

    size = os.path.getsize(output_path)
    print(f"\nDone. {output_path}  ({size} bytes)")
    print(f"Upload to your repo's packages/ directory and update index.json.")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Build a RPCortex .pkg archive')
    parser.add_argument('source', help='Package source directory (must contain package.cfg)')
    parser.add_argument('-o', '--output', help='Output .pkg filename', default=None)
    args = parser.parse_args()
    make_pkg(args.source, args.output)
