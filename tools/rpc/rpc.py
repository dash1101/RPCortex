#!/usr/bin/env python3
"""Run the rpc tool straight from a checkout, without installing it:

    python3 tools/rpc/rpc.py info
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rpctool.cli import main  # noqa: E402

sys.exit(main())
