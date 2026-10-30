#!/usr/bin/env python3
"""Cut a release: one command from "notes written" to "on GitHub and the sites".

    tools/release.py status               what the next release would be
    tools/release.py notes                the release notes as GitHub will show them
    tools/release.py cut                  dry run: every step is printed, nothing changes
    tools/release.py cut --publish        build, write, commit, tag, push, create the release
    tools/release.py label beta|rc|final  what the next release is called: v2.01-beta, v2.03-rc, v2.05
    tools/release.py bump iter            v2.11 -> v2.12   (edits os/kernel/kernel.h)
    tools/release.py bump major "Name"    v2.11 -> v3.00   (new codename)

WHERE THINGS COME FROM
----------------------
  version    os/kernel/kernel.h  (RPC_OS_MAJOR, RPC_OS_ITER, RPC_OS_SUFFIX, RPC_OS_CODENAME):
             the number and label the NEXT release will carry
  build      the commit count at the time the images are built
  notes      the `## Unreleased` section of CHANGELOG.md
  boards     releases/boards.json (hand edited: which boards have run which build)

WHAT IT WRITES
--------------
  releases/latest.json     the manifest a device reads for `update check`
  releases/*.bin, *.uf2    the images that manifest points at
  releases/releases.json   the history the websites read (one entry per release)
  CHANGELOG.md             `## Unreleased` becomes a dated section; a new empty one is added
  out/release-<tag>/       versioned copies of the images and SHA256SUMS, uploaded to GitHub

EVERY release published takes the next number, whatever its label: v2.01-beta, then v2.02-beta,
then v2.03-rc, v2.04-rc2, then v2.05 once it is no longer a beta. The tag is the version as written,
so there is no build in a tag; the build is in the title and the notes. After a release is cut, kernel.h
is stepped to the next number (an rc counts up to rc2, rc3, ...) and that is committed. A tag is
never moved or deleted by this tool.

Only the standard library is needed, so it runs on a fresh checkout.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL_H = os.path.join(ROOT, "os", "kernel", "kernel.h")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")
RELEASES = os.path.join(ROOT, "releases")
BOARDS = ("pico2_w", "pico_w", "pico2", "pico")
RAW = "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/"
REPO_URL = "https://github.com/dash1101/RPCortex"
SECTIONS = (("added", "Added"), ("changed", "Changed"), ("fixed", "Fixed"),
            ("limitations", "Known limitations"))


# --- the version -------------------------------------------------------------

def read_version(text=None):
    text = text if text is not None else open(KERNEL_H).read()

    def grab(name):
        m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, text)
        if not m:
            raise SystemExit("kernel.h has no %s" % name)
        return m.group(1)
    return {"major": grab("RPC_OS_MAJOR"), "iter": grab("RPC_OS_ITER"),
            "suffix": grab("RPC_OS_SUFFIX"), "codename": grab("RPC_OS_CODENAME")}


def display(v):
    return "v%s.%s%s" % (v["major"], v["iter"], v["suffix"])


def tag_for(v, build=None):
    """The tag is the version as written. Every release takes a new number, so it is unique."""
    return display(v)


def comparable(v, build):
    """What a device compares: MAJOR.ITER.BUILD."""
    return "%s.%s.%s" % (v["major"], v["iter"], build)


def title_for(v, build):
    return "RPCortex %s (build %s)" % (display(v), build)


def bump(v, what, name=None):
    v = dict(v)
    if what == "iter":
        v["iter"] = "%02d" % (int(v["iter"]) + 1)
    elif what == "major":
        if not name:
            raise SystemExit("a new major needs its codename: bump major \"Name\"")
        v["major"], v["iter"], v["codename"] = str(int(v["major"]) + 1), "00", name
    return v


def rc_after(suffix):
    """The label of the release candidate that follows one labelled `suffix` ('' if it was not an rc)."""
    m = re.match(r"^-rc(\d*)$", suffix or "")
    if not m:
        return "-rc"
    return "-rc%d" % ((int(m.group(1)) if m.group(1) else 1) + 1)


def relabel(v, what, last_published=None):
    """What the next release is called. `last_published` is the label of the one before it, for counting rcs."""
    v = dict(v)
    if what == "beta":
        v["suffix"] = "-beta"
    elif what == "final":
        v["suffix"] = ""
    elif what == "rc":
        v["suffix"] = rc_after(last_published) if (last_published or "").startswith("-rc") else "-rc"
    else:
        raise SystemExit("a label is beta, rc or final")
    return v


def next_after(v):
    """The version to carry once `v` has been published: the next number, and the next rc if it was one."""
    n = bump(v, "iter")
    if (v["suffix"] or "").startswith("-rc"):
        n["suffix"] = rc_after(v["suffix"])
    return n


def write_version(v):
    s = open(KERNEL_H).read()
    for key, name in (("major", "RPC_OS_MAJOR"), ("iter", "RPC_OS_ITER"),
                      ("suffix", "RPC_OS_SUFFIX"), ("codename", "RPC_OS_CODENAME")):
        s, n = re.subn(r'(#define\s+%s\s+")[^"]*(")' % name, r'\g<1>%s\g<2>' % v[key], s)
        if n != 1:
            raise SystemExit("could not rewrite %s" % name)
    open(KERNEL_H, "w").write(s)


# --- the changelog -----------------------------------------------------------

def parse_unreleased(text):
    """The Unreleased section as {summary, added, changed, fixed, limitations}."""
    m = re.search(r"^## Unreleased[ \t]*\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    if not m:
        raise SystemExit("CHANGELOG.md has no '## Unreleased' section")
    body = m.group(1)
    out = {"summary": "", "added": [], "changed": [], "fixed": [], "limitations": []}
    sm = re.search(r"^Summary:[ \t]*(.+)$", body, re.M)
    if sm:
        out["summary"] = sm.group(1).strip()
    heads = {label.lower(): key for key, label in SECTIONS}
    cur = None
    for line in body.splitlines():
        h = re.match(r"^###\s+(.+?)\s*$", line)
        if h:
            cur = heads.get(h.group(1).lower())
            continue
        b = re.match(r"^[-*]\s+(.*)$", line)
        if b and cur:
            out[cur].append(b.group(1).strip())
        elif cur and out[cur] and line.startswith("  ") and line.strip():
            out[cur][-1] += " " + line.strip()       # a wrapped bullet
    return out


def check_notes(notes):
    problems = []
    if not notes["summary"] or "[" in notes["summary"]:
        problems.append("Unreleased has no 'Summary:' line (one plain sentence, no placeholders)")
    if not any(notes[k] for k in ("added", "changed", "fixed")):
        problems.append("Unreleased lists nothing under Added, Changed or Fixed")
    return problems


def roll_changelog(text, heading):
    """Turn Unreleased into a dated section and put a fresh, empty one above it."""
    m = re.search(r"^## Unreleased[ \t]*\n", text, re.M)
    fresh = ("## Unreleased\n\nSummary: \n\n### Added\n\n### Changed\n\n### Fixed\n\n"
             "### Known limitations\n\n")
    return text[:m.start()] + fresh + "## " + heading + "\n" + text[m.end():]


def render_notes(entry, boards):
    """The GitHub release body."""
    n = entry["notes"]
    lines = [entry["summary"], ""]
    for key, label in SECTIONS:
        if n[key]:
            lines += ["### " + label, ""] + ["- " + i for i in n[key]] + [""]
    lines += ["### Install", "",
              "Hold BOOTSEL, plug the board in, and copy the `.uf2` for it onto the drive that "
              "appears. A running v2 device updates itself with `update install`.", "",
              "| Board | Image | Size | Run on hardware |", "|---|---|---|---|"]
    status = {b["id"]: b for b in boards.get("boards", [])}
    for a in entry["assets"]:
        if a["kind"] != "uf2":
            continue
        b = status.get(a["board"], {})
        ran = ("yes, build %s" % b["verified_build"]) if b.get("verified_build") else "not yet"
        lines.append("| %s | `%s` | %d KB | %s |" % (b.get("name", a["board"]), a["file"],
                                                    a["size"] // 1024, ran))
    lines += ["", "Checksums are in `SHA256SUMS`. The full history is in the "
              "[changelog](%s/blob/main/CHANGELOG.md)." % REPO_URL]
    return "\n".join(lines) + "\n"


# --- the plumbing ------------------------------------------------------------

def sh(args, **kw):
    return subprocess.run(args, cwd=ROOT, check=True, text=True, **kw)


def out(args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def build_number():
    return out(["git", "rev-list", "--count", "HEAD"])


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_json(path, default):
    return json.load(open(path)) if os.path.exists(path) else default


def last_label(hist=None):
    """The label of the most recently published release ('' for a final, None if nothing is published)."""
    hist = hist if hist is not None else load_json(os.path.join(RELEASES, "releases.json"), {"releases": []})
    if not hist["releases"]:
        return None
    m = re.match(r"^v\d+\.\d+(-.*)?$", hist["releases"][0]["display"])
    return (m.group(1) or "") if m else ""


def plan(msg, dry):
    print(("  would: " if dry else "  doing: ") + msg)


# --- commands ----------------------------------------------------------------

def cmd_status(_a):
    v = read_version()
    build = build_number()
    notes = parse_unreleased(open(CHANGELOG).read())
    print("version   %s   codename %s   build %s (the commit count now)" % (display(v), v["codename"], build))
    print("tag       %s   (the next release; the last published was %s)" % (
        tag_for(v, build), (load_json(os.path.join(RELEASES, "releases.json"), {"releases": []})["releases"] or [{"display": "none"}])[0]["display"]))
    print("device    compares %s" % comparable(v, build))
    print("notes     %d added, %d changed, %d fixed, %d limitations" %
          tuple(len(notes[k]) for k in ("added", "changed", "fixed", "limitations")))
    for p in check_notes(notes):
        print("  ! " + p)
    hist = load_json(os.path.join(RELEASES, "releases.json"), {"releases": []})
    if hist["releases"]:
        last = hist["releases"][0]
        print("last      %s (build %s, %s)" % (last["display"], last["build"], last["date"]))
    boards = load_json(os.path.join(RELEASES, "boards.json"), {"boards": []})
    for b in boards["boards"]:
        print("board     %-8s %s" % (b["id"], b["status"] + (" at build %s" % b["verified_build"]
                                                              if b.get("verified_build") else "")))


def cmd_notes(_a):
    v = read_version()
    notes = parse_unreleased(open(CHANGELOG).read())
    entry = {"summary": notes["summary"], "notes": notes,
             "assets": [{"kind": "uf2", "board": b, "file": "rpcortex-%s-%s.uf2" % (tag_for(v, "N"), b), "size": 0}
                        for b in BOARDS]}
    print(render_notes(entry, load_json(os.path.join(RELEASES, "boards.json"), {})))


def cmd_bump(a):
    v = read_version()
    nv = bump(v, a.what, a.name)
    print("%s -> %s  (%s)" % (display(v), display(nv), nv["codename"]))
    if a.what == "major" and not a.yes:
        print("dry run; add --yes to write kernel.h")
        return
    write_version(nv)
    print("kernel.h updated. Review it, then commit.")


def cmd_label(a):
    v = read_version()
    nv = relabel(v, a.what, last_label())
    print("%s -> %s" % (display(v), display(nv)))
    write_version(nv)
    print("kernel.h updated. Review it, then commit.")


def cmd_cut(a):
    dry = not a.publish
    v = read_version()
    text = open(CHANGELOG).read()
    notes = parse_unreleased(text)
    problems = check_notes(notes)
    if problems:
        raise SystemExit("not ready:\n  " + "\n  ".join(problems))

    if out(["git", "status", "--porcelain", "--untracked-files=no"]):
        raise SystemExit("the working tree has uncommitted changes; commit them first so the "
                         "images are built from a commit")
    if out(["git", "rev-parse", "--abbrev-ref", "HEAD"]) != "main":
        raise SystemExit("releases are cut from main")

    build = build_number()                       # the count the images are built at
    commit = out(["git", "rev-parse", "HEAD"])
    tag = tag_for(v, build)
    title = title_for(v, build)
    date = datetime.date.today().isoformat()
    print("%s\n  tag %s   build %s   commit %s\n  device compares %s\n" %
          (title, tag, build, commit[:8], comparable(v, build)))
    if out(["git", "tag", "--list", tag]):
        raise SystemExit("tag %s already exists; a tag is never moved" % tag)
    hist0 = load_json(os.path.join(RELEASES, "releases.json"), {"releases": []})
    if any(r["display"] == display(v) for r in hist0["releases"]):
        raise SystemExit("%s is already in releases.json; bump the number" % display(v))

    # 1. images
    if a.skip_build:
        plan("use the images already in out/ (they must be for build %s)" % build, dry)
    else:
        for b in BOARDS:
            plan("./build.sh %s   (image checks and the host suites)" % b, dry)
            if not dry:
                sh(["./build.sh", b])

    # 2. the images must be this version and build
    want = comparable(v, build).encode()
    assets, ota = [], []
    outdir = os.path.join(ROOT, "out", "release-" + tag)
    for b in BOARDS:
        binp = os.path.join(ROOT, "out", "rpcortex-v2-%s.bin" % b)
        uf2p = os.path.join(ROOT, "out", "rpcortex-v2-%s.uf2" % b)
        if not os.path.exists(binp) or not os.path.exists(uf2p):
            if dry and not a.skip_build:
                continue
            raise SystemExit("out/ has no image for %s" % b)
        data = open(binp, "rb").read()
        if want not in data:
            if dry and not a.skip_build:
                continue
            raise SystemExit("out/rpcortex-v2-%s.bin was not built as %s; rebuild it" % (b, want.decode()))
        for kind, path in (("bin", binp), ("uf2", uf2p)):
            name = "rpcortex-%s-%s.%s" % (tag, b, kind)
            assets.append({"board": b, "kind": kind, "file": name,
                           "size": os.path.getsize(path), "sha256": sha256(path),
                           "url": "%s/releases/download/%s/%s" % (REPO_URL, tag, name), "_src": path})
        ota.append({"name": b, "ver": comparable(v, build),
                    "desc": "%s %s for %s" % (title, v["codename"], b),
                    "author": "dash1101", "arch": "armv6m" if b in ("pico", "pico_w") else "armv8m",
                    "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    "url": RAW + "rpcortex-v2-%s.bin" % b})
    plan("write releases/latest.json and the images it points at (%d boards)" % len(ota), dry)
    plan("write out/release-%s/ with versioned names and SHA256SUMS" % tag, dry)

    entry = {"tag": tag, "display": display(v), "version": "%s.%s" % (v["major"], v["iter"]),
             "build": int(build), "commit": commit, "codename": v["codename"], "date": date,
             "prerelease": bool(v["suffix"]), "title": title, "summary": notes["summary"],
             "notes": {k: notes[k] for k, _ in SECTIONS},
             "assets": [{k: x[k] for k in x if k != "_src"} for x in assets],
             "url": "%s/releases/tag/%s" % (REPO_URL, tag)}
    boards = load_json(os.path.join(RELEASES, "boards.json"), {"boards": []})
    body = render_notes(entry, boards)

    # 3. files
    hist_path = os.path.join(RELEASES, "releases.json")
    hist = load_json(hist_path, {"schema": 1, "releases": []})
    hist["releases"] = [r for r in hist["releases"] if r["tag"] != tag]
    hist["releases"].insert(0, entry)
    heading = ("%s (build %s) — %s" % (display(v), build, date)) if v["suffix"] \
        else ("%s — %s" % (display(v), date))
    plan("CHANGELOG.md: Unreleased becomes '## %s'" % heading, dry)
    plan("releases/releases.json: add %s (the sites read this)" % tag, dry)

    if dry:
        print("\n--- release notes ---\n" + body)
        print("dry run: nothing was changed. Add --publish to do it.")
        return

    os.makedirs(outdir, exist_ok=True)
    sums = []
    for x in assets:
        dst = os.path.join(outdir, x["file"])
        open(dst, "wb").write(open(x["_src"], "rb").read())
        sums.append("%s  %s" % (x["sha256"], x["file"]))
    open(os.path.join(outdir, "SHA256SUMS"), "w").write("\n".join(sums) + "\n")
    for b in BOARDS:
        for kind in ("bin", "uf2"):
            src = os.path.join(ROOT, "out", "rpcortex-v2-%s.%s" % (b, kind))
            if os.path.exists(src):
                open(os.path.join(RELEASES, "rpcortex-v2-%s.%s" % (b, kind)), "wb").write(open(src, "rb").read())
    json.dump({"name": "RPCortex releases", "format": 2, "maintainer": "dash1101", "packages": ota},
              open(os.path.join(RELEASES, "latest.json"), "w"), indent=2)
    json.dump(hist, open(hist_path, "w"), indent=2, ensure_ascii=False)
    open(os.path.join(outdir, "NOTES.md"), "w").write(body)
    open(CHANGELOG, "w").write(roll_changelog(text, heading))

    # 4. publish
    sh(["git", "add", "CHANGELOG.md", "releases"])
    sh(["git", "commit", "-q", "-m", "release: %s" % title])
    sh(["git", "tag", tag])
    sh(["git", "push", "origin", "main"])
    sh(["git", "push", "origin", tag])
    gh = ["gh", "release", "create", tag, "--title", title, "--notes-file", os.path.join(outdir, "NOTES.md")]
    if v["suffix"]:
        gh.append("--prerelease")
    else:
        gh.append("--latest")
    files = sorted(os.path.join(outdir, f) for f in os.listdir(outdir) if f != "NOTES.md")
    sh(gh + files)
    print("\npublished %s\n%s" % (tag, entry["url"]))

    # the next release carries the next number
    nxt = next_after(v)
    write_version(nxt)
    sh(["git", "add", KERNEL_H])
    sh(["git", "commit", "-q", "-m", "version: the next release is %s" % display(nxt)])
    sh(["git", "push", "origin", "main"])
    print("kernel.h is now %s" % display(nxt))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    sub.add_parser("notes").set_defaults(fn=cmd_notes)
    c = sub.add_parser("cut")
    c.add_argument("--publish", action="store_true", help="really do it (default: a dry run)")
    c.add_argument("--skip-build", action="store_true", help="use the images already in out/")
    c.set_defaults(fn=cmd_cut)
    b = sub.add_parser("bump")
    b.add_argument("what", choices=("iter", "major"))
    b.add_argument("name", nargs="?", help="the codename, for a new major")
    b.add_argument("--yes", action="store_true")
    b.set_defaults(fn=cmd_bump)
    lb = sub.add_parser("label")
    lb.add_argument("what", choices=("beta", "rc", "final"))
    lb.set_defaults(fn=cmd_label)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
