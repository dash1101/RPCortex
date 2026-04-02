# Desc: Package manager for RPCortex - Nebula OS
# File: /Core/pkgmgr.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta4
# Author: dash1101
#
# Package format:  .pkg  (standard ZIP archive renamed)
# Contents:
#   <pkg_dir>/package.cfg   — package metadata (required)
#   <pkg_dir>/main.py       — main entry point (optional)
#   <pkg_dir>/...           — any other files
#
# package.cfg keys:
#   pkg.name   — display name           e.g. 'HelloWorld'
#   pkg.dev    — author
#   pkg.ver    — version string         e.g. '1.0.0'
#   pkg.dir    — install path           e.g. '/Packages/HelloWorld'
#   pkg.desc   — short description
#   pkg.cmd    — shell command          e.g. 'hello:/Packages/HelloWorld/main.py:hello'
#
# Repo index format (JSON hosted on web):
#   {
#     "name": "Repo Display Name",
#     "packages": [
#       {"name":"X", "ver":"1.0.0", "desc":"...", "url":"...", "author":"..."}
#     ]
#   }

import uos
import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import ok, warn, error, info, multi

PACKAGES_DIR = '/Packages'
PROGRAMS_LP  = '/Core/Launchpad/programs.lp'
PKG_BASE     = '/Nebula/pkg'
REPOS_CFG    = '/Nebula/pkg/repos.cfg'
CACHE_DIR    = '/Nebula/pkg/cache'

# ---------------------------------------------------------------------------
# Minimal ZIP reader  (stored and deflate-compressed entries)
# ---------------------------------------------------------------------------

def _u32(data, off):
    return (data[off] | (data[off+1] << 8) |
            (data[off+2] << 16) | (data[off+3] << 24))

def _u16(data, off):
    return data[off] | (data[off+1] << 8)


def _extract_zip_entries(data):
    """Yield (filename, bytes) pairs from a ZIP archive."""
    offset = 0
    length = len(data)

    while offset < length - 4:
        sig = data[offset:offset+4]

        if sig == b'PK\x01\x02' or sig == b'PK\x05\x06':
            break

        if sig != b'PK\x03\x04':
            offset += 1
            continue

        comp_method = _u16(data, offset + 8)
        comp_size   = _u32(data, offset + 18)
        fname_len   = _u16(data, offset + 26)
        extra_len   = _u16(data, offset + 28)

        fname    = data[offset+30 : offset+30+fname_len].decode('utf-8')
        data_off = offset + 30 + fname_len + extra_len
        raw      = data[data_off : data_off + comp_size]

        if comp_method == 0:
            file_data = raw
        elif comp_method == 8:
            file_data = None
            # Attempt 1: zlib (MicroPython v1.19+)
            try:
                import zlib
                file_data = zlib.decompress(raw, -15)
            except ImportError:
                pass
            except Exception as e:
                error("zlib decompress error '{}': {}".format(fname, e))
            # Attempt 2: uzlib (older MicroPython)
            if file_data is None:
                try:
                    import uzlib
                    file_data = uzlib.decompress(raw, -15)
                except ImportError:
                    pass
                except Exception as e:
                    error("uzlib decompress error '{}': {}".format(fname, e))
            # Neither available
            if file_data is None:
                error("Cannot decompress '{}' — no zlib/uzlib module.".format(fname))
                error("Rebuild with stored compression: python repo/make_pkg.py <dir>")
        else:
            warn("Skipping '{}' — unsupported compression {}".format(fname, comp_method))
            file_data = None

        if file_data is not None:
            yield fname, file_data

        offset = data_off + comp_size

# ---------------------------------------------------------------------------
# Filesystem helpers
# ---------------------------------------------------------------------------

def _makedirs(path):
    parts = [p for p in path.split('/') if p]
    current = ''
    for part in parts:
        current += '/' + part
        try:
            uos.mkdir(current)
        except OSError:
            pass


def _rmtree(path):
    try:
        stat = uos.stat(path)
    except OSError:
        return
    if stat[0] & 0x4000:
        for entry in uos.listdir(path):
            _rmtree(path.rstrip('/') + '/' + entry)
        uos.rmdir(path)
    else:
        uos.remove(path)


def _ensure_dirs():
    _makedirs(PKG_BASE)
    _makedirs(CACHE_DIR)

# ---------------------------------------------------------------------------
# package.cfg parser
# ---------------------------------------------------------------------------

def _parse_cfg(text):
    result = {}
    for line in text.split('\n'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if ':' in line:
            k, v = line.split(':', 1)
            result[k.strip()] = v.strip().strip("'\"")
    return result

# ---------------------------------------------------------------------------
# programs.lp helpers
# ---------------------------------------------------------------------------

def _register_command(cmd_entry):
    try:
        try:
            with open(PROGRAMS_LP, 'r') as f:
                existing = f.read()
        except OSError:
            existing = ''
        if cmd_entry in existing:
            return
        with open(PROGRAMS_LP, 'a') as f:
            f.write(cmd_entry + '\n')
        ok("Registered command: {}".format(cmd_entry.split(':')[0]))
    except Exception as e:
        warn("Could not register command: {}".format(e))


def _unregister_commands(pkg_name):
    try:
        with open(PROGRAMS_LP, 'r') as f:
            lines = f.readlines()
        kept = [l for l in lines if pkg_name.lower() not in l.lower() or l.startswith('#')]
        with open(PROGRAMS_LP, 'w') as f:
            f.write(''.join(kept))   # writelines() absent in MicroPython TextIOWrapper
    except Exception:
        pass


def _clear_reg_keys(reg_keys_str):
    """Delete registry keys listed in pkg.reg_keys (comma-separated dot-notation)."""
    try:
        import regedit
    except ImportError:
        warn("Could not import regedit — registry keys not cleared.")
        return
    keys = [k.strip() for k in reg_keys_str.split(',') if k.strip()]
    cleared = 0
    for key in keys:
        try:
            regedit.delete(key)
            cleared += 1
        except Exception as e:
            warn("  Could not delete {}: {}".format(key, e))
    if cleared:
        ok("Removed {} registry key(s).".format(cleared))

# ---------------------------------------------------------------------------
# Repo management
# ---------------------------------------------------------------------------

def _read_repos():
    """Return list of repo URL strings."""
    try:
        with open(REPOS_CFG, 'r') as f:
            lines = f.read().split('\n')
        return [l.strip() for l in lines if l.strip() and not l.strip().startswith('#')]
    except OSError:
        return []


def _write_repos(urls):
    _ensure_dirs()
    with open(REPOS_CFG, 'w') as f:
        f.write('# RPCortex package repository list\n')
        f.write('# One URL per line pointing to a repo index.json\n')
        for url in urls:
            f.write(url + '\n')


def repo_add(url):
    if not url:
        warn("Usage: pkg repo add <url>")
        return False
    repos = _read_repos()
    if url in repos:
        warn("Repo already in list: {}".format(url))
        return False
    repos.append(url)
    _write_repos(repos)
    ok("Added repo: {}".format(url))
    return True


def repo_remove(url):
    if not url:
        warn("Usage: pkg repo remove <url>")
        return False
    repos = _read_repos()
    before = len(repos)
    repos = [r for r in repos if r != url]
    if len(repos) == before:
        warn("Repo not found: {}".format(url))
        return False
    _write_repos(repos)
    ok("Removed repo: {}".format(url))
    return True


def repo_list():
    repos = _read_repos()
    if not repos:
        multi("  (no repos configured)")
        multi("  Add one with: pkg repo add <url>")
        return
    multi("  {:<4} {}".format("ID", "URL"))
    multi("  " + "-" * 60)
    for i, url in enumerate(repos):
        multi("  {:<4} {}".format(i, url))
    multi("")
    multi("  {} repo(s) configured.".format(len(repos)))

# ---------------------------------------------------------------------------
# Cache helpers
# ---------------------------------------------------------------------------

def _cache_path(idx):
    return CACHE_DIR + '/' + str(idx) + '.json'


def _load_cache(idx):
    """Load a cached repo index. Returns list of package dicts or []."""
    try:
        import ujson
        with open(_cache_path(idx), 'r') as f:
            data = ujson.load(f)
        return data.get('packages', [])
    except Exception:
        return []

# ---------------------------------------------------------------------------
# Version comparison
# ---------------------------------------------------------------------------

def _ver_tuple(v):
    try:
        return tuple(int(x) for x in v.split('.'))
    except Exception:
        return (0,)


def _ver_gt(a, b):
    """Return True if version string a is greater than b."""
    return _ver_tuple(a) > _ver_tuple(b)

# ---------------------------------------------------------------------------
# Update — refresh repo cache from network
# ---------------------------------------------------------------------------

def update():
    """Fetch the index from every configured repo and cache it locally."""
    repos = _read_repos()
    if not repos:
        warn("No repos configured. Add one with: pkg repo add <url>")
        return False

    _ensure_dirs()
    import gc
    gc.collect()
    import net

    updated = 0
    for i, url in enumerate(repos):
        gc.collect()
        info("[{}/{}] Fetching: {}".format(i + 1, len(repos), url))
        dest = _cache_path(i)
        try:
            status, size = net.wget(url, dest=dest, verbose=False)
            if status == 200:
                ok("  Cached {} bytes from repo {}.".format(size, i))
                updated += 1
            else:
                error("  HTTP {} from repo: {}".format(status, url))
                try:
                    uos.remove(dest)
                except OSError:
                    pass
        except Exception as e:
            error("  Failed to fetch repo {}: {}".format(i, e))

    if updated:
        ok("Update complete. {}/{} repo(s) refreshed.".format(updated, len(repos)))
    else:
        warn("No repos were updated successfully.")
    return updated > 0

# ---------------------------------------------------------------------------
# Search
# ---------------------------------------------------------------------------

def search(query):
    """Search cached repo indexes for packages matching query."""
    if not query:
        warn("Usage: pkg search <query>")
        return

    repos = _read_repos()
    if not repos:
        warn("No repos configured. Run 'pkg repo add <url>' then 'pkg update'.")
        return

    query_l = query.lower()
    found = 0
    multi("  {:<20} {:<10} {:<12} {}".format("NAME", "VERSION", "AUTHOR", "DESCRIPTION"))
    multi("  " + "-" * 65)

    for i in range(len(repos)):
        pkgs = _load_cache(i)
        for pkg in pkgs:
            name = pkg.get('name', '')
            desc = pkg.get('desc', '')
            if query_l in name.lower() or query_l in desc.lower():
                multi("  {:<20} {:<10} {:<12} {}".format(
                    name,
                    pkg.get('ver', '?'),
                    pkg.get('author', '?'),
                    desc[:35]
                ))
                found += 1

    if found == 0:
        multi("  No packages found matching '{}'.".format(query))
        multi("  Run 'pkg update' to refresh the repo cache.")
    else:
        multi("")
        multi("  {} result(s). Install with: pkg install <name>".format(found))

# ---------------------------------------------------------------------------
# Available — list all packages in the repo cache
# ---------------------------------------------------------------------------

def available():
    """List every package in all cached repo indexes."""
    repos = _read_repos()
    if not repos:
        warn("No repos configured. Add one with: pkg repo add <url>")
        return

    multi("  {:<20} {:<10} {:<12} {}".format("NAME", "VERSION", "AUTHOR", "DESCRIPTION"))
    multi("  " + "-" * 65)

    total = 0
    for i in range(len(repos)):
        pkgs = _load_cache(i)
        if not pkgs:
            continue
        for pkg in pkgs:
            multi("  {:<20} {:<10} {:<12} {}".format(
                pkg.get('name', '?'),
                pkg.get('ver',  '?'),
                pkg.get('author', '?'),
                pkg.get('desc', '')[:35]
            ))
            total += 1

    if total == 0:
        multi("  No packages in cache. Run 'pkg update' to fetch repo index.")
    else:
        multi("")
        multi("  {} package(s) available. Install with: pkg install <name>".format(total))

# ---------------------------------------------------------------------------
# Install — local file
# ---------------------------------------------------------------------------

def install(archive_path):
    """Install a package from a local .pkg archive."""
    if not archive_path:
        warn("Usage: pkg install <path/to/file.pkg>")
        return False

    if not archive_path.startswith('/'):
        try:
            archive_path = uos.getcwd().rstrip('/') + '/' + archive_path
        except Exception:
            pass

    info("Reading archive: {}".format(archive_path))
    try:
        with open(archive_path, 'rb') as f:
            data = f.read()
    except OSError as e:
        error("Cannot read '{}': {}".format(archive_path, e))
        return False

    return _install_from_data(data)


def _install_from_data(data):
    """Extract and install package from raw ZIP bytes."""
    entries = list(_extract_zip_entries(data))
    if not entries:
        error("Archive appears to be empty or invalid.")
        return False

    # Find package.cfg
    cfg_data  = None
    cfg_entry = None
    for fname, fdata in entries:
        if fname.endswith('package.cfg') and not fname.endswith('/'):
            cfg_data  = fdata.decode('utf-8') if isinstance(fdata, (bytes, bytearray)) else fdata
            cfg_entry = fname
            break

    if cfg_data is None:
        error("No package.cfg found in archive.")
        return False

    meta     = _parse_cfg(cfg_data)
    pkg_name = meta.get('pkg.name', 'Unknown')
    pkg_dir  = meta.get('pkg.dir', PACKAGES_DIR + '/' + pkg_name)
    pkg_ver  = meta.get('pkg.ver', '?')
    pkg_desc = meta.get('pkg.desc', '')

    info("Package     : {}  v{}".format(pkg_name, pkg_ver))
    info("Install dir : {}".format(pkg_dir))
    if pkg_desc:
        info("Description : {}".format(pkg_desc))

    # Check for existing install
    try:
        uos.stat(pkg_dir)
        warn("'{}' is already installed.".format(pkg_name))
        warn("Remove first with: pkg remove {}".format(pkg_name))
        return False
    except OSError:
        pass

    # Strip top-level ZIP directory prefix
    prefix = ''
    if cfg_entry and '/' in cfg_entry:
        prefix = cfg_entry.split('/')[0] + '/'

    ok("Extracting to '{}'...".format(pkg_dir))
    _makedirs(pkg_dir)

    for fname, fdata in entries:
        rel = fname[len(prefix):] if fname.startswith(prefix) else fname
        if not rel:
            continue

        full_path = pkg_dir.rstrip('/') + '/' + rel

        if rel.endswith('/'):
            _makedirs(full_path.rstrip('/'))
            continue

        parent = '/'.join(full_path.split('/')[:-1])
        if parent:
            _makedirs(parent)

        try:
            with open(full_path, 'wb') as f:
                f.write(fdata if isinstance(fdata, (bytes, bytearray)) else fdata.encode())
            info("  + {}".format(full_path))
        except OSError as e:
            error("  Failed to write '{}': {}".format(full_path, e))

    cmd_entry = meta.get('pkg.cmd')
    if cmd_entry:
        _register_command(cmd_entry)

    ok("Package '{}' v{} installed.".format(pkg_name, pkg_ver))
    return True

# ---------------------------------------------------------------------------
# Install — online by name
# ---------------------------------------------------------------------------

def install_online(name):
    """Look up a package by name in cached repo indexes and install it."""
    repos = _read_repos()
    if not repos:
        warn("No repos configured. Add one with: pkg repo add <url>")
        return False

    # Find the package in cache
    pkg_url = None
    for i in range(len(repos)):
        for pkg in _load_cache(i):
            if pkg.get('name', '').lower() == name.lower():
                pkg_url = pkg.get('url')
                break
        if pkg_url:
            break

    if not pkg_url:
        error("'{}' not found in repo cache. Run 'pkg update' first.".format(name))
        return False

    info("Found '{}'. Downloading...".format(name))
    import net

    tmp = PKG_BASE + '/tmp_install.pkg'
    _ensure_dirs()
    try:
        status, size = net.wget(pkg_url, dest=tmp, verbose=True)
    except Exception as e:
        error("Download failed: {}".format(e))
        return False

    if status != 200:
        error("Server returned HTTP {} — aborting.".format(status))
        try:
            uos.remove(tmp)
        except OSError:
            pass
        return False

    ok("Downloaded {} bytes. Installing...".format(size))
    import gc
    gc.collect()
    try:
        with open(tmp, 'rb') as f:
            data = f.read()
        gc.collect()
        result = _install_from_data(data)
    except Exception as e:
        error("Install failed: {}".format(e))
        result = False
    finally:
        try:
            uos.remove(tmp)
        except OSError:
            pass

    return result

# ---------------------------------------------------------------------------
# Upgrade
# ---------------------------------------------------------------------------

def upgrade():
    """Check all installed packages against the repo cache and upgrade any outdated ones."""
    repos = _read_repos()
    if not repos:
        warn("No repos configured.")
        return

    # Build a dict of latest available versions from cache
    available = {}
    for i in range(len(repos)):
        for pkg in _load_cache(i):
            name = pkg.get('name', '')
            ver  = pkg.get('ver', '0')
            url  = pkg.get('url', '')
            if name and url:
                if name not in available or _ver_gt(ver, available[name]['ver']):
                    available[name] = {'ver': ver, 'url': url}

    if not available:
        warn("Repo cache is empty. Run 'pkg update' first.")
        return

    # Check installed packages
    try:
        entries = uos.listdir(PACKAGES_DIR)
    except OSError:
        info("No packages installed.")
        return

    upgraded = 0
    for entry in sorted(entries):
        cfg_path = PACKAGES_DIR + '/' + entry + '/package.cfg'
        try:
            with open(cfg_path, 'r') as f:
                meta = _parse_cfg(f.read())
        except OSError:
            continue

        name        = meta.get('pkg.name', '')
        cur_ver     = meta.get('pkg.ver', '0')
        avail       = available.get(name)

        if avail and _ver_gt(avail['ver'], cur_ver):
            info("Upgrading {} {} -> {}...".format(name, cur_ver, avail['ver']))
            if uninstall(name):
                import net
                tmp = PKG_BASE + '/tmp_upgrade.pkg'
                _ensure_dirs()
                try:
                    status, size = net.wget(avail['url'], dest=tmp, verbose=False)
                    if status == 200:
                        with open(tmp, 'rb') as f:
                            data = f.read()
                        if _install_from_data(data):
                            upgraded += 1
                except Exception as e:
                    error("Upgrade of '{}' failed: {}".format(name, e))
                finally:
                    try:
                        uos.remove(tmp)
                    except OSError:
                        pass
        else:
            info("  {} v{} — up to date.".format(name, cur_ver))

    if upgraded:
        ok("{} package(s) upgraded.".format(upgraded))
    else:
        ok("All packages are up to date.")

# ---------------------------------------------------------------------------
# Remove
# ---------------------------------------------------------------------------

def uninstall(pkg_name):
    """Remove an installed package by name."""
    if not pkg_name:
        warn("Usage: pkg remove <name>")
        return False

    target_dir = None
    try:
        for entry in uos.listdir(PACKAGES_DIR):
            candidate = PACKAGES_DIR + '/' + entry + '/package.cfg'
            try:
                with open(candidate, 'r') as f:
                    meta = _parse_cfg(f.read())
                if meta.get('pkg.name', '').lower() == pkg_name.lower():
                    target_dir = PACKAGES_DIR + '/' + entry
                    break
            except OSError:
                pass
    except OSError:
        error("Cannot access '{}'.".format(PACKAGES_DIR))
        return False

    if target_dir is None:
        error("Package '{}' not found.".format(pkg_name))
        return False

    # Read package.cfg before removal (rmtree deletes it)
    reg_keys = ''
    try:
        with open(target_dir + '/package.cfg', 'r') as f:
            meta_check = _parse_cfg(f.read())
        if meta_check.get('pkg.builtin') == 'true':
            error("'{}' is a built-in package and cannot be removed.".format(pkg_name))
            return False
        reg_keys = meta_check.get('pkg.reg_keys', '')
    except OSError:
        pass

    info("Removing '{}'...".format(target_dir))
    _rmtree(target_dir)
    _unregister_commands(pkg_name)

    if reg_keys:
        _clear_reg_keys(reg_keys)

    ok("Package '{}' removed.".format(pkg_name))
    return True

# ---------------------------------------------------------------------------
# List / Info
# ---------------------------------------------------------------------------

def list_pkgs():
    """List all installed packages."""
    try:
        entries = uos.listdir(PACKAGES_DIR)
    except OSError:
        multi("  (no packages installed)")
        return

    found = 0
    multi("  {:<20} {:<10} {}".format("NAME", "VERSION", "DESCRIPTION"))
    multi("  " + "-" * 56)
    for entry in sorted(entries):
        cfg_path = PACKAGES_DIR + '/' + entry + '/package.cfg'
        try:
            with open(cfg_path, 'r') as f:
                meta = _parse_cfg(f.read())
            name = meta.get('pkg.name', entry)
            ver  = meta.get('pkg.ver',  '?')
            desc = meta.get('pkg.desc', '')
            if meta.get('pkg.builtin') == 'true':
                desc = (desc + ' [built-in]') if desc else '[built-in]'
            multi("  {:<20} {:<10} {}".format(name, ver, desc))
            found += 1
        except OSError:
            pass

    if found == 0:
        multi("  (no packages installed)")
    else:
        multi("")
        multi("  {} package(s) installed.".format(found))


def info_pkg(pkg_name):
    """Show detailed info for an installed package."""
    if not pkg_name:
        warn("Usage: pkg info <name>")
        return

    try:
        for entry in uos.listdir(PACKAGES_DIR):
            cfg_path = PACKAGES_DIR + '/' + entry + '/package.cfg'
            try:
                with open(cfg_path, 'r') as f:
                    meta = _parse_cfg(f.read())
                if meta.get('pkg.name', '').lower() == pkg_name.lower():
                    multi("")
                    multi("  === {} ===".format(meta.get('pkg.name', '?')))
                    for k, v in meta.items():
                        multi("  {:12} {}".format(k.replace('pkg.', '') + ':', v))
                    multi("")
                    return
            except OSError:
                pass
    except OSError:
        pass
    error("Package '{}' not found.".format(pkg_name))
