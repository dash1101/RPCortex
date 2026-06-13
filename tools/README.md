# RPCortex tools

PC-side companion tools for an RPCortex device.

## `rpc_comm.py` — RPCortex Communicator

Talk to a board running RPCortex over USB serial — change registry values, sync
the clock, move files both ways, install packages, and update/reinstall the OS,
all from your computer. It uses MicroPython's raw REPL with RPCortex's `rawrepl`
escape, so it works whether the device is at the shell or the login screen.

### Setup
```
pip install pyserial
```

### Examples
```
python rpc_comm.py ports                                   # find your port
python rpc_comm.py --port COM7 info                        # version + platform
python rpc_comm.py --port COM7 time-sync                   # set clock to this PC
python rpc_comm.py --port COM7 reg get System.Owner
python rpc_comm.py --port COM7 reg set System.TZ_Offset -5
python rpc_comm.py --port COM7 ls /Users/root
python rpc_comm.py --port COM7 push notes.txt /Users/root/notes.txt
python rpc_comm.py --port COM7 pull /Pulsar/Logs/latest.log latest.log
python rpc_comm.py --port COM7 rm /Users/root/notes.txt
python rpc_comm.py --port COM7 pkg-install dist/sysmon.pkg # local .pkg
python rpc_comm.py --port COM7 pkg-online SysMon           # from the repo (WiFi)
python rpc_comm.py --port COM7 os-update RPC-Pulsar-b9-Stable.rpc
python rpc_comm.py --port COM7 os-update-online            # fetch latest + apply
```

### Notes
- **Close PuTTY/Thonny first** — only one program can own the serial port.
- `os-update` / `os-update-online` use the device's own `rpc_install` extractor,
  which **preserves** `/Users` and `/Pulsar` (accounts, settings, WiFi, packages);
  the device reboots when finished. For a clean wipe use the Web Installer's
  "Clean install" option instead.
- `os-update-online` downloads the image **on this PC** (from
  `rpc.novalabs.app/releases/latest.json`) and pushes it — the device doesn't
  need WiFi. `pkg-online` does need device WiFi.
- `shell "<cmd>"` runs a single RPCortex shell command via the live engine —
  handy but advanced; the other commands talk to MicroPython directly and are
  the robust path.
