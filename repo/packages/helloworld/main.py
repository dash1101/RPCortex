# Desc: HelloWorld — sample RPCortex package
# File: /Packages/HelloWorld/main.py
# Version: 1.0.0
# Author: dash1101

import sys
if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import ok, multi


def hello(args=None):
    multi("")
    multi("  Hello from HelloWorld!")
    multi("  RPCortex package system is working correctly.")
    if args:
        multi("  You said: " + args)
    multi("")
    ok("HelloWorld v1.0.0")
