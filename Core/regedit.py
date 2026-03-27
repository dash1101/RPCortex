# Desc: Registry — system configuration read/write for RPCortex - Nebula OS
# File: /Core/regedit.py
# Last Updated: 3/26/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta2
# Author: dash1101
#
# The registry is an INI-style config file at /Nebula/Registry/registry.cfg.
# Sections are delimited by [SectionName] headers; keys use colon separators.
#
# This module caches the parsed config in memory after the first load.
# Writes go through to disk immediately and update the cache in place.

CONFIG_FILE = "/Nebula/Registry/registry.cfg"

# ---------------------------------------------------------------------------
# In-memory config cache
# ---------------------------------------------------------------------------

_cache = None   # dict of dicts; None = not yet loaded


def _invalidate():
    """Drop the cached config so the next read() re-parses from disk."""
    global _cache
    _cache = None


def load_config():
    """Parse registry.cfg and return (and cache) the config dict."""
    global _cache
    if _cache is not None:
        return _cache
    config = {}
    section = None
    try:
        with open(CONFIG_FILE, 'r') as file:
            for line in file:
                line = line.strip()
                if line.startswith('[') and line.endswith(']'):
                    section = line[1:-1]
                    config[section] = {}
                elif section and ':' in line:
                    key, value = line.split(':', 1)
                    config[section][key.strip()] = value.strip()
    except OSError:
        pass
    _cache = config
    return config


def save_config(config):
    """Write the full config dict to disk and update the cache."""
    global _cache
    with open(CONFIG_FILE, 'w') as file:
        for section, items in config.items():
            file.write('[{}]\n'.format(section))
            for key, value in items.items():
                file.write('{}: {}\n'.format(key, value))
            file.write('\n')
    _cache = config


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def save(key, value):
    """Write a single key. Creates the section if it doesn't exist."""
    config = load_config()
    section, key = key.split('.', 1)
    if section not in config:
        config[section] = {}
    config[section][key] = value
    save_config(config)


def read(key):
    """Read a single key. Returns None if the section or key is missing."""
    config = load_config()
    section, key = key.split('.', 1)
    return config.get(section, {}).get(key, None)


def delete(key):
    """Delete a key; removes the section if it becomes empty."""
    config = load_config()
    section, key_name = key.split('.', 1)
    if section in config and key_name in config[section]:
        del config[section][key_name]
        if not config[section]:
            del config[section]
        save_config(config)
