"""Themes, the same tokens as Orbit's (its web/index.html), and where the
choice is remembered.

A theme is a set of colours by role. The screen asks for a role (text, muted,
accent, ok, ...) and the curses layer turns it into the nearest colour the
terminal has: the exact one where it takes 24-bit colour, the nearest of the
xterm 256 where it has those, the nearest of the basic 8 otherwise, and
bold, dim and reverse where there is no colour at all. The terminal's own
palette is never changed.
"""
import json
import os
import sys

# bg, raise, line, line-hi, text, muted, legend, accent, accent-ink, ok, warn, down, off, sel
THEMES = {
    "tui": dict(bg="#1c1c1e", raise_="#262629", line="#3d3d42", line_hi="#5b5b62",
                text="#d6d6d8", muted="#8c8c93", legend="#9a9aa1", accent="#f0668e",
                accent_ink="#1c1c1e", ok="#23a55a", warn="#f0b232", down="#f23f43",
                off="#6d6f78", sel="#e8e8ea"),
    "phosphor": dict(bg="#070c08", raise_="#0d1710", line="#1d3a23", line_hi="#2f6a3b",
                     text="#a8ffb0", muted="#4f9a5a", legend="#62b96e", accent="#39ff7a",
                     accent_ink="#04140a", ok="#39ff7a", warn="#e6ff5c", down="#ff5c5c",
                     off="#2f5a36", sel="#a8ffb0"),
    "amber": dict(bg="#120d05", raise_="#1d1508", line="#3e2e12", line_hi="#6b4e1c",
                  text="#ffcf7a", muted="#a07a3c", legend="#c4913f", accent="#ffb000",
                  accent_ink="#120d05", ok="#ffd36b", warn="#ff8a3d", down="#ff4d3d",
                  off="#5e4722", sel="#ffcf7a"),
    "ember": dict(bg="#1a1412", raise_="#251c18", line="#3f302a", line_hi="#644a3f",
                  text="#ecd9cf", muted="#9a8276", legend="#b08d7e", accent="#ff7a45",
                  accent_ink="#1a1412", ok="#9ccc65", warn="#ffc24b", down="#ff4f4f",
                  off="#6b574e", sel="#f3e3da"),
    "frost": dict(bg="#1b2028", raise_="#232a35", line="#36404f", line_hi="#4f5d72",
                  text="#dbe3ee", muted="#8592a6", legend="#93a4bb", accent="#7cc4e8",
                  accent_ink="#12171e", ok="#8fcf8a", warn="#ebcb7d", down="#e0707a",
                  off="#5d6879", sel="#e6edf5"),
    "lilac": dict(bg="#1d1a24", raise_="#26222f", line="#3b3448", line_hi="#594e6d",
                  text="#e4ddf0", muted="#9389a6", legend="#a596c2", accent="#b794f6",
                  accent_ink="#1d1a24", ok="#7bd88f", warn="#f2c66d", down="#f7768e",
                  off="#625a73", sel="#efe9f9"),
    "mono": dict(bg="#111111", raise_="#1c1c1c", line="#333333", line_hi="#555555",
                 text="#e0e0e0", muted="#8a8a8a", legend="#9e9e9e", accent="#ffffff",
                 accent_ink="#111111", ok="#cfcfcf", warn="#a8a8a8", down="#ffffff",
                 off="#4a4a4a", sel="#ffffff"),
    "ocean": dict(bg="#0d1b24", raise_="#13252f", line="#20394a", line_hi="#315670",
                  text="#cfe6f0", muted="#6f93a5", legend="#7fa8bb", accent="#2ec4b6",
                  accent_ink="#062027", ok="#3ddc97", warn="#ffbf46", down="#ff6b6b",
                  off="#3b5566", sel="#d8eef7"),
    "paper": dict(bg="#f3f0e8", raise_="#e9e5da", line="#c4bdae", line_hi="#8f877a",
                  text="#2a2826", muted="#6b655e", legend="#6a645c", accent="#c2366b",
                  accent_ink="#ffffff", ok="#16733d", warn="#8f5b12", down="#c53030",
                  off="#a39d92", sel="#2a2826"),
}
ORDER = ("tui", "phosphor", "amber", "ember", "frost", "lilac", "mono", "ocean", "paper")
DEFAULT = "tui"

# The roles the screen draws with, each the theme token for its colour.
ROLES = ("text", "muted", "legend", "accent", "line", "line_hi", "ok", "warn", "down", "off",
         "sel")


def next_theme(name):
    i = ORDER.index(name) if name in ORDER else -1
    return ORDER[(i + 1) % len(ORDER)]


def rgb(hexstr):
    h = hexstr.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def _dist(a, b):
    # Weighted for the eye, which is most sensitive to green.
    return 3 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 2 * (a[2] - b[2]) ** 2


_CUBE = (0, 95, 135, 175, 215, 255)


def xterm256(hexstr):
    """The nearest xterm-256 colour index (the 6x6x6 cube or the grey ramp;
    0-15 are left alone, since terminals redefine them)."""
    c = rgb(hexstr)
    best, best_d = 16, None
    for i in range(216):
        r, g, b = _CUBE[i // 36], _CUBE[(i // 6) % 6], _CUBE[i % 6]
        d = _dist(c, (r, g, b))
        if best_d is None or d < best_d:
            best, best_d = 16 + i, d
    for i in range(24):
        v = 8 + 10 * i
        d = _dist(c, (v, v, v))
        if d < best_d:
            best, best_d = 232 + i, d
    return best


# The eight basic colours as most terminals draw them.
BASIC = ((0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0),
         (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229))


def basic8(hexstr):
    """The nearest of the eight basic colours, by hue where there is one: a
    grey goes to black or white, anything with colour to the nearest colour
    that is not grey, so an accent stays an accent."""
    c = rgb(hexstr)
    lo, hi = min(c), max(c)
    if hi - lo < 40:
        return 0 if hi < 128 else 7
    best, best_d = 1, None
    for i in range(1, 7):
        d = _dist(c, BASIC[i])
        if best_d is None or d < best_d:
            best, best_d = i, d
    return best


def is_light(name):
    r, g, b = rgb(THEMES.get(name, THEMES[DEFAULT])["bg"])
    return (r * 299 + g * 587 + b * 114) / 1000 > 128


# --- remembering the choice --------------------------------------------------------

APP_DIR = "rpc-tui"


def config_dir(platform=None, environ=None):
    """Where the settings live: %APPDATA% on Windows, ~/Library/Application
    Support on macOS, $XDG_CONFIG_HOME (or ~/.config) elsewhere."""
    platform = platform or sys.platform
    env = os.environ if environ is None else environ
    home = env.get("HOME") or os.path.expanduser("~")
    if platform.startswith("win"):
        base = env.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
    elif platform == "darwin":
        base = os.path.join(home, "Library", "Application Support")
    else:
        base = env.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    return os.path.join(base, APP_DIR)


def config_path(**kw):
    return os.path.join(config_dir(**kw), "config.json")


def load_config(path=None):
    path = path or config_path()
    try:
        with open(path, "r") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(cfg, path=None):
    """Write the settings; a failure (a read-only home) is not worth stopping for."""
    path = path or config_path()
    try:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(cfg, f, indent=2)
        os.replace(tmp, path)
        return True
    except OSError:
        return False
