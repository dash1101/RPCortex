#!/usr/bin/env python3
"""Checks for tools/release.py: the parts that decide what a release says and is called."""
import os, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release as r

KH = '''#define RPC_OS_MAJOR    "2"
#define RPC_OS_ITER     "00"
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

## v2.00 (beta)

old text
'''

class Version(unittest.TestCase):
    def test_reads_kernel_h(self):
        v = r.read_version(KH)
        self.assertEqual((v["major"], v["iter"], v["suffix"], v["codename"]), ("2", "00", "-beta", "Vela II"))
    def test_display_and_tags(self):
        v = r.read_version(KH)
        self.assertEqual(r.display(v), "v2.00-beta")
        self.assertEqual(r.tag_for(v, 486), "v2.00-beta")
        self.assertEqual(r.comparable(v, 486), "2.00.486")
        self.assertEqual(r.title_for(v, 486), "RPCortex v2.00-beta (build 486)")
    def test_a_tag_is_the_version_and_the_title_carries_the_build(self):
        v = r.bump(r.relabel(r.read_version(KH), "final"), "iter")
        self.assertEqual(r.tag_for(v, 1203), "v2.01")
        self.assertEqual(r.title_for(v, 1203), "RPCortex v2.01 (build 1203)")
    def test_iterations_do_not_stop_at_nine(self):
        v = r.read_version(KH); v["suffix"] = ""; v["iter"] = "09"
        self.assertEqual(r.display(r.bump(v, "iter")), "v2.10")
        v["iter"] = "99"
        self.assertEqual(r.display(r.bump(v, "iter")), "v2.100")
    def test_major_resets_iteration_and_needs_a_name(self):
        v = r.read_version(KH); v["suffix"] = ""; v["iter"] = "11"
        n = r.bump(v, "major", "Orion")
        self.assertEqual((r.display(n), n["codename"]), ("v3.00", "Orion"))
        self.assertEqual(r.title_for(n, 1), "RPCortex v3.00 (build 1)")
        with self.assertRaises(SystemExit): r.bump(v, "major")

class Labels(unittest.TestCase):
    def v(self, iter_, suffix):
        v = r.read_version(KH); v["iter"] = iter_; v["suffix"] = suffix
        return v
    def test_beta_then_final_drops_the_label_and_keeps_counting(self):
        self.assertEqual(r.display(r.relabel(self.v("02", "-beta"), "final")), "v2.02")
        self.assertEqual(r.display(r.relabel(self.v("02", ""), "beta")), "v2.02-beta")
    def test_a_first_rc_is_plain_and_later_ones_count(self):
        self.assertEqual(r.display(r.relabel(self.v("02", "-beta"), "rc", "-beta")), "v2.02-rc")
        self.assertEqual(r.display(r.relabel(self.v("03", "-rc"), "rc", "-rc")), "v2.03-rc2")
        self.assertEqual(r.display(r.relabel(self.v("04", "-rc2"), "rc", "-rc2")), "v2.04-rc3")
        self.assertEqual(r.display(r.relabel(self.v("05", "-rc3"), "rc", None)), "v2.05-rc")
    def test_after_a_release_the_next_number_is_next_and_an_rc_counts_up(self):
        self.assertEqual(r.display(r.next_after(self.v("01", "-beta"))), "v2.02-beta")
        self.assertEqual(r.display(r.next_after(self.v("03", "-rc"))), "v2.04-rc2")
        self.assertEqual(r.display(r.next_after(self.v("04", "-rc2"))), "v2.05-rc3")
        self.assertEqual(r.display(r.next_after(self.v("05", ""))), "v2.06")
    def test_the_label_of_the_last_published_release(self):
        h = {"releases": [{"display": "v2.03-rc2"}, {"display": "v2.02"}]}
        self.assertEqual(r.last_label(h), "-rc2")
        self.assertEqual(r.last_label({"releases": [{"display": "v2.02"}]}), "")
        self.assertIsNone(r.last_label({"releases": []}))
    def test_the_rc_after_something_that_was_not_an_rc_is_the_first(self):
        for was in ("-beta", "", None):
            self.assertEqual(r.rc_after(was), "-rc")
    def test_a_bad_label_is_refused(self):
        with self.assertRaises(SystemExit): r.relabel(self.v("01", ""), "gold")

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
        out = r.roll_changelog(LOG, "v2.00-beta (build 486) — 2026-10-30")
        self.assertEqual(out.count("## Unreleased"), 1)
        self.assertIn("## v2.00-beta (build 486) — 2026-10-30\n\nSummary: Faster boot", out)
        self.assertIn("## v2.00 (beta)\n\nold text", out)
        self.assertEqual(r.parse_unreleased(out)["summary"], "")
    def test_notes_render(self):
        n = r.parse_unreleased(LOG)
        e = {"summary": n["summary"], "notes": n, "assets": [{"kind": "uf2", "board": "pico2_w", "file": "f.uf2", "size": 2048}]}
        body = r.render_notes(e, {"boards": [{"id": "pico2_w", "name": "Pico 2 W", "verified_build": 486}]})
        self.assertIn("### Fixed", body); self.assertIn("- A hang", body)
        self.assertIn("| Pico 2 W | `f.uf2` | 2 KB | yes, build 486 |", body)

if __name__ == "__main__":
    unittest.main()
