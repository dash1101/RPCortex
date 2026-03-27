# Desc: Package manager shell command handler for RPCortex - Nebula OS
# File: /Core/Launchpad/pkg.py
# Last Updated: 3/25/2026
# Lang: MicroPython, English
# Version: v0.3.0
# Author: dash1101
#
# Loaded once into a cached exec scope by launchpad.py.
# Dispatches 'pkg' subcommands to Core/pkgmgr.py.
#
# Usage:
#   pkg install <name>             Install a package by name (from repo cache)
#   pkg install <path/to/file.pkg> Install a local .pkg archive
#   pkg remove  <name>             Remove an installed package
#   pkg list                       List installed packages
#   pkg info    <name>             Show package details
#   pkg search  <query>            Search repo cache
#   pkg update                     Refresh repo cache from network
#   pkg upgrade                    Upgrade outdated packages
#   pkg repo list                  List configured repos
#   pkg repo add    <url>          Add a repo
#   pkg repo remove <url>          Remove a repo

import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, info, multi


def pkg(args=None):
    if not args:
        _pkg_help()
        return

    parts = args.strip().split(None, 1)
    sub   = parts[0].lower()
    rest  = parts[1].strip() if len(parts) > 1 else None

    # --- install ---
    if sub == 'install':
        if not rest:
            warn("Usage: pkg install <name>  OR  pkg install <path/to/file.pkg>")
            return
        import pkgmgr
        # Treat as local file if it has a slash or ends with .pkg
        if rest.startswith('/') or rest.endswith('.pkg'):
            ok_flag = pkgmgr.install(rest)
        else:
            ok_flag = pkgmgr.install_online(rest)
        # Reload commands dict so the new command is available immediately
        # without requiring a reboot.
        if ok_flag:
            _reload = globals().get('_load_commands')
            if _reload:
                try:
                    _reload()
                except Exception:
                    pass

    # --- remove ---
    elif sub in ('remove', 'uninstall', 'rm'):
        if not rest:
            warn("Usage: pkg remove <name>")
            return
        import pkgmgr
        if pkgmgr.uninstall(rest):
            _cmds   = globals().get('_commands')
            _reload = globals().get('_load_commands')
            if _cmds is not None and _reload:
                try:
                    _cmds.clear()
                    _reload()
                except Exception:
                    pass

    # --- list ---
    elif sub == 'list':
        import pkgmgr
        pkgmgr.list_pkgs()

    # --- info ---
    elif sub == 'info':
        if not rest:
            warn("Usage: pkg info <name>")
            return
        import pkgmgr
        pkgmgr.info_pkg(rest)

    # --- available ---
    elif sub in ('available', 'browse', 'all'):
        import pkgmgr
        pkgmgr.available()

    # --- search ---
    elif sub == 'search':
        if not rest:
            warn("Usage: pkg search <query>")
            return
        import pkgmgr
        pkgmgr.search(rest)

    # --- update ---
    elif sub == 'update':
        import gc
        try:
            _cmd_cache.clear()
        except NameError:
            pass
        gc.collect()
        gc.collect()
        import pkgmgr
        pkgmgr.update()

    # --- upgrade ---
    elif sub == 'upgrade':
        import pkgmgr
        pkgmgr.upgrade()
        _reload = globals().get('_load_commands')
        if _reload:
            try:
                _reload()
            except Exception:
                pass

    # --- repo ---
    elif sub == 'repo':
        _pkg_repo(rest)

    else:
        warn("Unknown subcommand '{}'. Run 'pkg' for usage.".format(sub))
        _pkg_help()


def _pkg_repo(args):
    if not args:
        import pkgmgr
        pkgmgr.repo_list()
        return

    parts = args.strip().split(None, 1)
    sub   = parts[0].lower()
    rest  = parts[1].strip() if len(parts) > 1 else None

    import pkgmgr

    if sub == 'list':
        pkgmgr.repo_list()
    elif sub == 'add':
        if not rest:
            warn("Usage: pkg repo add <url>")
            return
        pkgmgr.repo_add(rest)
    elif sub in ('remove', 'rm', 'del'):
        if not rest:
            warn("Usage: pkg repo remove <url>")
            return
        pkgmgr.repo_remove(rest)
    else:
        warn("Unknown repo subcommand '{}'. Options: list, add, remove".format(sub))


def _pkg_help():
    info("=== Package Manager  (pkg) ===")
    multi("")
    multi("  Package commands:")
    multi("    pkg install <name>         Install a package by name from repo")
    multi("    pkg install <file.pkg>     Install a local .pkg archive")
    multi("    pkg remove  <name>         Remove an installed package")
    multi("    pkg list                   List installed packages")
    multi("    pkg info    <name>         Show package details")
    multi("")
    multi("  Repo commands:")
    multi("    pkg available              List all packages in the repo cache")
    multi("    pkg search  <query>        Search repo cache")
    multi("    pkg update                 Refresh repo cache from network")
    multi("    pkg upgrade                Upgrade outdated packages")
    multi("    pkg repo list              List configured repos")
    multi("    pkg repo add    <url>      Add a repo index URL")
    multi("    pkg repo remove <url>      Remove a repo")
    multi("")
