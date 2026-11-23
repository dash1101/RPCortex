"""Reading what the device's commands print, into plain values.

Each parser takes the text of one EXEC (colour codes and all; the firmware
puts C_* codes inside some format strings, `ps` and `pkg list` among them)
and returns what it could find, leaving out what it could not. Nothing here
talks to the device, so all of it is tested on its own.
"""
import re

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][A-Za-z0-9]|\x1b[=>78]")


def clean(text):
    """Text without colour codes or carriage returns."""
    return ANSI.sub("", text).replace("\r", "")


def _lines(text):
    return [ln.rstrip() for ln in clean(text).split("\n")]


def _kb(pattern, text):
    m = re.search(pattern + r"\s*:\s*(\d+)\s*KB", text)
    return int(m.group(1)) if m else None


# --- ver ----------------------------------------------------------------------

def parse_ver(text):
    """`ver`: {'version', 'codename', 'commit', 'stage', 'board', 'arch',
    'features', 'custom'} as far as they are there."""
    out = {}
    for ln in _lines(text):
        s = ln.strip()
        m = re.match(r"RPCortex\s+(\S+)(?:\s+\(([^)]*)\))?", s)
        if m and "version" not in out:
            out["version"] = m.group(1)
            if m.group(2):
                # Firmware before 2026-11-21 printed "(build N)" here.
                b = re.match(r"build\s+(\d+)", m.group(2))
                if b:
                    out["build"] = b.group(1)
                else:
                    out["codename"] = m.group(2)
            continue
        m = re.match(r"Commit:\s*(\S+)(?:\s+\(([^)]*)\))?", s)
        if m:
            out["commit"] = m.group(1)
            if m.group(2):
                out["stage"] = m.group(2)
            continue
        m = re.search(r"Platform:\s*(\S+)(?:\s+\(([^)]*)\))?", s)
        if m:
            out["board"] = m.group(1)
            if m.group(2):
                out["arch"] = m.group(2)
            continue
        m = re.match(r"Features:\s*(.*)$", s)
        if m:
            feats = m.group(1)
            c = re.search(r"\(custom image:\s*([^)]*)\)", feats)
            if c:
                out["custom"] = c.group(1).strip()
                feats = feats[:c.start()]
            out["features"] = [f.strip() for f in re.split(r"[,\s]+", feats) if f.strip()]
    return out


def parse_sysinfo(text):
    """`sysinfo`: the device's name ('device_id', the host part of its
    prompt) and its owner, where they are set."""
    out = {}
    for key, label in (("device_id", "Device ID"), ("owner", "Owner")):
        m = re.search(r"^\s*" + label + r"\s*:\s*(.+?)\s*$", clean(text), re.M)
        if m:
            out[key] = m.group(1)
    return out


# --- status: uptime, meminfo, df, wifi ---------------------------------------

STATUS_MARK_MEM = "--rpctui-mem--"
STATUS_MARK_DF = "--rpctui-df--"
# One EXEC for the three, so a refresh is one round trip. `meminfo` and `df`
# both print "Total :" and "Free :", so each part is read on its own.
STATUS_LINE = "uptime ; echo {} ; meminfo ; echo {} ; df".format(STATUS_MARK_MEM, STATUS_MARK_DF)


def split_marks(text, *marks):
    """The text before the first mark, then the text after each mark."""
    parts = []
    rest = clean(text)
    for m in marks:
        i = rest.find(m)
        if i < 0:
            parts.append(rest)
            rest = ""
            continue
        parts.append(rest[:i])
        rest = rest[i + len(m):]
    parts.append(rest)
    return parts


def parse_uptime(text):
    """'Uptime: 1h 2m 3s' -> seconds, or None."""
    m = re.search(r"Uptime:\s*(?:(\d+)h)?\s*(?:(\d+)m)?\s*(?:(\d+)s)?", clean(text))
    if not m or not any(m.groups()):
        return None
    h, mi, s = (int(g) if g else 0 for g in m.groups())
    return h * 3600 + mi * 60 + s


def human_uptime(secs):
    if secs is None:
        return "-"
    d, rem = divmod(int(secs), 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    if d:
        return "{}d {}h {}m".format(d, h, m)
    if h:
        return "{}h {}m".format(h, m)
    if m:
        return "{}m {}s".format(m, s)
    return "{}s".format(s)


def parse_meminfo(text):
    t = clean(text)
    out = {"total": _kb("Total", t), "free": _kb("Free", t), "largest": _kb("Largest block", t)}
    return {k: v for k, v in out.items() if v is not None}


def parse_df(text):
    t = clean(text)
    out = {"total": _kb("Total", t), "used": _kb("Used", t), "free": _kb("Free", t)}
    m = re.search(r"Used\s*:\s*\d+\s*KB\s*\((\d+)%\)", t)
    if m:
        out["pct"] = int(m.group(1))
    return {k: v for k, v in out.items() if v is not None}


def parse_status(text):
    up, mem, df = split_marks(text, STATUS_MARK_MEM, STATUS_MARK_DF)
    return {"uptime": parse_uptime(up), "mem": parse_meminfo(mem), "df": parse_df(df)}


def parse_wifi(text):
    """`wifi status`: {'state': 'off'|'up'|'down'|'none', 'link', 'network',
    'address', 'signal'}. 'none' is a board or image without Wi-Fi."""
    t = clean(text)
    low = t.lower()
    if "without wi-fi" in low or "no wireless hardware" in low or "not a command" in low:
        return {"state": "none", "note": t.strip().splitlines()[0] if t.strip() else ""}
    out = {}
    m = re.search(r"Radio\s*:\s*(\S+)", t)
    if m:
        out["radio"] = m.group(1)
    for key, label in (("link", "Link"), ("network", "Network"), ("address", "Address"),
                       ("signal", "Signal")):
        m = re.search(r"^\s*" + label + r"\s*:\s*(.+)$", t, re.M)
        if m:
            out[key] = m.group(1).strip()
    if out.get("radio") == "off":
        out["state"] = "off"
    elif "address" in out:
        out["state"] = "up"
    elif out:
        out["state"] = "down"
    else:
        out["state"] = "unknown"
    if "only an admin" in low or "needs an admin" in low:
        out["state"] = "unknown"
        out["note"] = "only an admin can see the Wi-Fi state"
    return out


# --- ps, kill, log --------------------------------------------------------------

class Task(object):
    __slots__ = ("pid", "name", "state", "core", "stack", "cpu")

    def __init__(self, pid, name, state, core="", stack="", cpu=""):
        self.pid, self.name, self.state = pid, name, state
        self.core, self.stack, self.cpu = core, stack, cpu

    def __repr__(self):
        return "Task({}, {!r}, {!r})".format(self.pid, self.name, self.state)


def parse_ps(text):
    """`ps`: (tasks, summary line). Columns: PID NAME STATE CORE STACK CPU."""
    tasks, summary = [], ""
    head = None
    for ln in _lines(text):
        s = ln.strip()
        if not s or set(s) <= set("-"):
            continue
        parts = s.split()
        if parts[0] == "PID":
            head = [p.upper() for p in parts]
            continue
        if re.match(r"^\d+ tasks?\b", s):
            summary = s           # "3 tasks · CPU 3% · 280 KB free", under the list
            continue
        if parts[0].isdigit() and len(parts) >= 3:
            if head and "STATE" in head and len(parts) >= len(head) - (1 if "PATH" in head else 0):
                row = dict(zip(head, parts))
                tasks.append(Task(int(parts[0]), row.get("NAME", ""), row.get("STATE", ""),
                                  row.get("CORE", ""), row.get("STACK", ""), row.get("CPU", "")))
            else:
                # Older layout: PID STATE CORE NAME.
                tasks.append(Task(int(parts[0]), parts[-1], parts[1],
                                  parts[2] if len(parts) > 3 else ""))
    return tasks, summary


LOG_MARK = "--rpctui-log--"
LOGS_LINE = "ps ; echo {} ; logdump".format(LOG_MARK)


def parse_log(text):
    """`logdump`: the lines, each (kind, text). kind is the tag in brackets
    ('!', '?', '@', ':') or '' where there is none."""
    out = []
    for ln in _lines(text):
        if not ln.strip():
            continue
        m = re.match(r"\s*\d+\.\d+\s+\[(.)\]", ln)
        kind = m.group(1) if m else ""
        if re.match(r"\s*\d+ lines?$", ln):
            continue
        out.append((kind, ln.strip()))
    return out


def parse_logs(text):
    ps, log = split_marks(text, LOG_MARK)
    tasks, summary = parse_ps(ps)
    return tasks, summary, parse_log(log)


# --- packages -----------------------------------------------------------------

def parse_pkgs(text):
    """`_pkgs` (PKGS_BEGIN / PKG:name:ver / PKGS_END): {name: version}, or
    None when the firmware does not have it."""
    t = clean(text)
    if "PKGS_BEGIN" not in t:
        return None
    out = {}
    for ln in t.split("\n"):
        ln = ln.strip()
        if ln.startswith("PKG:"):
            parts = ln.split(":", 2)
            if len(parts) == 3:
                out[parts[1]] = parts[2]
    return out


def parse_pkg_list(text):
    """`pkg list`: {name: version}."""
    out = {}
    for ln in _lines(text):
        s = ln.strip()
        if not s or s.startswith("[") or s.endswith(":") or s.lower().startswith("usage"):
            continue
        parts = s.split()
        if len(parts) >= 2 and re.match(r"^[A-Za-z0-9_.-]+$", parts[0]):
            out[parts[0]] = parts[1]
        elif len(parts) == 1 and re.match(r"^[A-Za-z0-9_.-]+$", parts[0]):
            out[parts[0]] = ""
    return out


# --- versions -------------------------------------------------------------------

def _digits(s, i):
    v = 0
    while i < len(s) and s[i].isdigit():
        v = min(v * 10 + int(s[i]), 0x7FFFFFFF)
        i += 1
    return v, i


def version_cmp(a, b):
    """repo_version_cmp() in os/core/repoindex.cpp: number by number; a
    component with a suffix sorts newer than one without; two suffixes compare
    as text; a component that ran out counts as zero."""
    a, b = a or "", b or ""
    i = j = 0
    while i < len(a) or j < len(b):
        va, i = _digits(a, i)
        vb, j = _digits(b, j)
        if va != vb:
            return -1 if va < vb else 1
        si, sj = i, j
        while i < len(a) and a[i] != ".":
            i += 1
        while j < len(b) and b[j] != ".":
            j += 1
        sa, sb = a[si:i], b[sj:j]
        if sa or sb:
            if not sa:
                return -1
            if not sb:
                return 1
            n = min(len(sa), len(sb))
            if sa[:n] != sb[:n]:
                return -1 if sa[:n] < sb[:n] else 1
            if len(sa) != len(sb):
                return -1 if len(sa) < len(sb) else 1
        if i < len(a) and a[i] == ".":
            i += 1
        if j < len(b) and b[j] == ".":
            j += 1
    return 0
