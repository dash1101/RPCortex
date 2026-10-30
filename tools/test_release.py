#!/usr/bin/env python3
"""Checks for tools/release.py: the parts that decide what a release says and is called."""
import os, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release as r

KH = '''#define RPC_OS_MAJOR    "2"
#define RPC_OS_ITER     "0"
#define RPC_OS_SUFFIX   "-beta"
#define RPC_OS_CODENAME "Vela II"
'''
LOG = '''# Changelog

## Unreleased

Summary: Faster boot, and a fix for a hang.

### Added

- A thing that was added
  and wrapped onto a second line.

### Fixed

- A hang
- Another

### Known limitations

- Something open

## v2.0 (beta)

old text
'''

class Version(unittest.TestCase):
    def test_reads_kernel_h(self):
        v = r.read_version(KH)
        self.assertEqual((v["major"], v["iter"], v["suffix"], v["codename"]), ("2", "0", "-beta", "Vela II"))
    def test_display_and_tags(self):
        v = r.read_version(KH)
        self.assertEqual(r.display(v), "v2.0-beta")
        self.assertEqual(r.tag_for(v, 486), "v2.0-beta.486")
        self.assertEqual(r.comparable(v, 486), "2.0.486")
        self.assertEqual(r.title_for(v, 486), "RPCortex v2.0-beta (build 486)")
    def test_stable_release_has_no_build_in_its_tag(self):
        v = r.bump(r.bump(r.read_version(KH), "ship"), "iter")
        self.assertEqual(r.tag_for(v, 1203), "v2.1")
        self.assertEqual(r.title_for(v, 1203), "RPCortex v2.1")
    def test_iterations_do_not_stop_at_nine(self):
        v = r.read_version(KH); v["suffix"] = ""; v["iter"] = "9"
        self.assertEqual(r.display(r.bump(v, "iter")), "v2.10")
    def test_major_resets_iteration_and_needs_a_name(self):
        v = r.read_version(KH); v["suffix"] = ""; v["iter"] = "11"
        n = r.bump(v, "major", "Orion")
        self.assertEqual((r.display(n), n["codename"]), ("v3.0", "Orion"))
        self.assertEqual(r.title_for(n, 1), "RPCortex v3.0 — Orion")
        with self.assertRaises(SystemExit): r.bump(v, "major")

class Notes(unittest.TestCase):
    def test_parse(self):
        n = r.parse_unreleased(LOG)
        self.assertEqual(n["summary"], "Faster boot, and a fix for a hang.")
        self.assertEqual(n["added"], ["A thing that was added and wrapped onto a second line."])
        self.assertEqual(n["fixed"], ["A hang", "Another"])
        self.assertEqual(n["limitations"], ["Something open"])
        self.assertEqual(n["changed"], [])
        self.assertEqual(r.check_notes(n), [])
    def test_an_empty_template_is_not_ready(self):
        empty = LOG.replace("Faster boot, and a fix for a hang.", "").replace("- A thing", "").replace("- A hang", "").replace("- Another", "")
        self.assertTrue(r.check_notes(r.parse_unreleased(empty)))
    def test_a_summary_with_nothing_listed_is_not_ready(self):
        p = r.check_notes({"summary": "Something happened.", "added": [], "changed": [], "fixed": [], "limitations": ["open"]})
        self.assertTrue(any("nothing" in x for x in p))
    def test_a_placeholder_summary_is_not_ready(self):
        self.assertTrue(r.check_notes({"summary": "[what changed]", "added": ["x"], "changed": [], "fixed": [], "limitations": []}))
    def test_roll_keeps_old_sections_and_adds_a_fresh_one(self):
        out = r.roll_changelog(LOG, "v2.0-beta (build 486) — 2026-10-30")
        self.assertEqual(out.count("## Unreleased"), 1)
        self.assertIn("## v2.0-beta (build 486) — 2026-10-30\n\nSummary: Faster boot", out)
        self.assertIn("## v2.0 (beta)\n\nold text", out)
        self.assertEqual(r.parse_unreleased(out)["summary"], "")
    def test_notes_render(self):
        n = r.parse_unreleased(LOG)
        e = {"summary": n["summary"], "notes": n, "assets": [{"kind": "uf2", "board": "pico2_w", "file": "f.uf2", "size": 2048}]}
        body = r.render_notes(e, {"boards": [{"id": "pico2_w", "name": "Pico 2 W", "verified_build": 486}]})
        self.assertIn("### Fixed", body); self.assertIn("- A hang", body)
        self.assertIn("| Pico 2 W | `f.uf2` | 2 KB | yes, build 486 |", body)

if __name__ == "__main__":
    unittest.main()
