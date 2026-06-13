# Desc: Recovery & diagnostic shell commands - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_recovery.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
#
# Diagnostics and repair tools, usable from the normal shell or recovery mode.
# Registered via its own recovery.lp so it still loads if system.lp is damaged.
#
#   fscheck            verify core OS files are present and non-empty
#   diag               quick health snapshot (RAM, flash, registry, version)
#   logdump [n]        print the current session log (last n lines if given)
#   regreset           delete registry.cfg so POST rebuilds it (keeps users/wifi)
#   pkgdisable <name>  disable a package without removing it
#   pkgenable  <name>  re-enable a disabled package

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi, inpt

# Core files expected on a healthy install.  Missing/empty entries are flagged.
_MANIFEST = (
    '/main.py',
    '/Core/RPCortex.py',
    '/Core/regedit.py',
    '/Core/initialization.py',
    '/Core/post.py',
    '/Core/launchpad.py',
    '/Core/usrmgmt.py',
    '/Core/net.py',
    '/Core/pulse.py',
    '/Core/pkgmgr.py',
    '/Core/rpc_install.py',
    '/Core/rpc_stub.py',
    '/Core/Launchpad/system.lp',
    '/Core/Launchpad/sys_fs.py',
    '/Core/Launchpad/sys_sys.py',
    '/Core/Launchpad/sys_net.py',
    '/Core/Launchpad/sys_user.py',
    '/Core/Launchpad/sys_text.py',
    '/Pulsar/Registry/registry.cfg',
    '/Pulsar/Registry/user.cfg',
)

_REGISTRY = '/Pulsar/Registry/registry.cfg'
_LOG      = '/Pulsar/Logs/latest.log'


def _stat_any(path):
    """stat the path, or its .mpy counterpart — a compiled build ships e.g.
    launchpad.mpy instead of launchpad.py. Returns (stat, real_path) or None."""
    try:
        return uos.stat(path), path
    except OSError:
        pass
    if path.endswith('.py'):
        alt = path[:-3] + '.mpy'
        try:
            return uos.stat(alt), alt
        except OSError:
            pass
    return None


def fscheck(args=None):
    """Verify core OS files exist and are non-empty (source OR compiled)."""
    info("Filesystem check — {} core files".format(len(_MANIFEST)))
    missing = 0
    empty = 0
    for path in _MANIFEST:
        res = _stat_any(path)
        if res is None:
            multi("  \033[91mMISSING\033[0m {}".format(path))
            missing += 1
            continue
        st, real = res
        if st[6] == 0:
            multi("  \033[93mEMPTY  \033[0m {}".format(real))
            empty += 1
        else:
            note = '  (.mpy)' if real.endswith('.mpy') else ''
            multi("  \033[92mOK     \033[0m {}{}".format(path, note))
    multi("")
    if missing == 0 and empty == 0:
        ok("All core files present.")
    else:
        error("{} missing, {} empty. Re-imaging is recommended (update reinstall).".format(missing, empty))


def diag(args=None):
    """Quick health snapshot."""
    import gc
    gc.collect()
    info("=== Diagnostics ===")
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    multi("  Free RAM    : {} KB / {} KB".format(free // 1024, (free + alloc) // 1024))
    try:
        sv = uos.statvfs('/')
        ftot = sv[0] * sv[2]
        ffree = sv[0] * sv[3]
        multi("  Free flash  : {} KB / {} KB".format(ffree // 1024, ftot // 1024))
    except OSError:
        multi("  Free flash  : unavailable")
    try:
        import regedit
        ver = regedit.read('Settings.Version') or '?'
        multi("  OS version  : {}".format(ver))
        multi("  Registry    : \033[92mreadable\033[0m")
    except Exception as e:
        multi("  Registry    : \033[91mERROR\033[0m ({})".format(e))
    multi("  Platform    : {}".format(sys.platform))


def logdump(args=None):
    """Print the current session log (optionally only the last n lines)."""
    n = None
    if args and args.strip():
        try:
            n = int(args.strip())
        except ValueError:
            warn("Usage: logdump [n]")
            return
    try:
        with open(_LOG, 'r') as f:
            lines = f.readlines()
    except OSError as e:
        error("Cannot read log '{}': {}".format(_LOG, e))
        return
    if n is not None:
        lines = lines[-n:]
    for line in lines:
        multi(line.rstrip('\n'))
    ok("{} line(s) from {}.".format(len(lines), _LOG))


def regreset(args=None):
    """Delete registry.cfg so POST rebuilds it from template on next boot.

    User accounts (user.cfg) and saved WiFi (networks.cfg) are NOT touched.
    """
    warn("This deletes the registry. POST rebuilds defaults on next boot.")
    warn("User accounts and saved WiFi are preserved.")
    if inpt("Type CONFIRM to reset the registry").strip() != 'CONFIRM':
        info("Cancelled.")
        return
    try:
        uos.remove(_REGISTRY)
        ok("Registry deleted. Reboot to rebuild defaults: reboot")
    except OSError as e:
        error("Could not delete registry: {}".format(e))


def _find_pkg_dir(name, suffix=''):
    base = '/Packages'
    try:
        for entry in uos.listdir(base):
            if entry.lower() == (name + suffix).lower():
                return base + '/' + entry
    except OSError:
        pass
    return None


def _pkg_cmd_names(pkg_dir):
    """Read the command names a package registers (from pkg.cmd in package.cfg)."""
    names = []
    try:
        with open(pkg_dir + '/package.cfg', 'r') as f:
            for line in f:
                line = line.strip()
                if line.startswith('pkg.cmd') and ':' in line:
                    val = line.split(':', 1)[1]
                    for entry in val.split(';'):
                        entry = entry.strip()
                        if entry:
                            names.append(entry.split(':', 1)[0].strip())
    except OSError:
        pass
    return names


def pkgdisable(args=None):
    """Disable a package by renaming its directory to <name>.disabled."""
    if not args or not args.strip():
        warn("Usage: pkgdisable <name>")
        return
    name = args.strip()
    target = _find_pkg_dir(name)
    if target is None:
        error("Package '{}' not found in /Packages.".format(name))
        return
    cmds = _pkg_cmd_names(target)   # read before the dir is renamed away
    try:
        uos.rename(target, target + '.disabled')
    except OSError as e:
        error("Could not disable '{}': {}".format(name, e))
        return
    # Drop it from the LIVE command table + cache so it stops working now,
    # not just after a reboot.
    live = globals().get('_commands')
    cache = globals().get('_cmd_cache')
    if live is not None:
        for c in cmds:
            if c in live:
                del live[c]
    if cache is not None:
        cache.clear()
    ok("Disabled '{}'. Its command(s) stop working immediately.".format(name))
    info("Re-enable with: pkgenable {}".format(name))


def pkgenable(args=None):
    """Re-enable a package previously disabled with pkgdisable."""
    if not args or not args.strip():
        warn("Usage: pkgenable <name>")
        return
    name = args.strip()
    target = _find_pkg_dir(name, '.disabled')
    if target is None:
        error("No disabled package '{}' found.".format(name))
        return
    restored = target[:-len('.disabled')]
    try:
        uos.rename(target, restored)
    except OSError as e:
        error("Could not re-enable '{}': {}".format(name, e))
        return
    # Re-register the command(s) live: clear cache + reload the command table.
    cache = globals().get('_cmd_cache')
    if cache is not None:
        cache.clear()
    reload = globals().get('_load_commands')
    if reload:
        try:
            reload()
        except Exception:
            pass
    ok("Re-enabled '{}'.".format(name))
