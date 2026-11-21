#!/usr/bin/env python3
"""Checks for tools/release.py: the parts that decide what a release says and is called."""
import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release as r

KH = '''#define RPC_OS_MAJOR    "2"
#define RPC_OS_MINOR    "0"
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
        self.assertEqual((v["major"], v["minor"], v["suffix"], v["codename"]), ("2", "0", "-beta", "Vela II"))
    def test_the_build_is_the_third_number(self):
        v = r.read_version(KH)
        self.assertEqual(r.display(v), "v2.0-beta")
        self.assertEqual(r.display(v, 640), "v2.0.640-beta")
        self.assertEqual(r.tag_for(v, 640), "v2.0.640-beta")
        self.assertEqual(r.comparable(v, 640), "2.0.640")
        self.assertEqual(r.title_for(v, 640), "RPCortex v2.0.640-beta")
    def test_final_drops_the_label(self):
        v = r.relabel(r.read_version(KH), "final")
        self.assertEqual(r.tag_for(v, 1203), "v2.0.1203")
        self.assertEqual(r.title_for(v, 1203), "RPCortex v2.0.1203")
    def test_minor_steps_and_does_not_stop_at_nine(self):
        v = r.read_version(KH)
        self.assertEqual(r.display(r.bump(v, "minor"), 700), "v2.1.700-beta")
        v["minor"] = "9"
        self.assertEqual(r.display(r.bump(v, "minor")), "v2.10-beta")
    def test_major_resets_minor_and_needs_a_name(self):
        v = r.read_version(KH); v["suffix"] = ""; v["minor"] = "3"
        n = r.bump(v, "major", "Orion")
        self.assertEqual((r.display(n, 1), n["codename"]), ("v3.0.1", "Orion"))
        with self.assertRaises(SystemExit): r.bump(v, "major")

class Labels(unittest.TestCase):
    def v(self, suffix):
        v = r.read_version(KH); v["suffix"] = suffix
        return v
    def test_beta_then_final_drops_the_label(self):
        self.assertEqual(r.display(r.relabel(self.v("-beta"), "final"), 9), "v2.0.9")
        self.assertEqual(r.display(r.relabel(self.v(""), "beta"), 9), "v2.0.9-beta")
    def test_a_first_rc_is_plain_and_later_ones_count(self):
        self.assertEqual(r.display(r.relabel(self.v("-beta"), "rc", "-beta")), "v2.0-rc")
        self.assertEqual(r.display(r.relabel(self.v("-rc"), "rc", "-rc")), "v2.0-rc2")
        self.assertEqual(r.display(r.relabel(self.v("-rc2"), "rc", "-rc2")), "v2.0-rc3")
        self.assertEqual(r.display(r.relabel(self.v("-rc3"), "rc", None)), "v2.0-rc")
    def test_after_a_release_only_an_rc_changes(self):
        b = self.v("-beta")
        self.assertEqual(r.next_after(b), b)                      # nothing to commit
        self.assertEqual(r.display(r.next_after(self.v("-rc"))), "v2.0-rc2")
        self.assertEqual(r.display(r.next_after(self.v("-rc2"))), "v2.0-rc3")
        f = self.v("")
        self.assertEqual(r.next_after(f), f)
    def test_the_label_of_the_last_published_release(self):
        self.assertEqual(r.last_label({"releases": [{"display": "v2.0.702-rc2"}]}), "-rc2")
        self.assertEqual(r.last_label({"releases": [{"display": "v2.0.731"}]}), "")
        h = {"releases": [{"display": "v2.03-rc2"}, {"display": "v2.02"}]}
        self.assertEqual(r.last_label(h), "-rc2")
        self.assertEqual(r.last_label({"releases": [{"display": "v2.02"}]}), "")
        self.assertIsNone(r.last_label({"releases": []}))
    def test_the_rc_after_something_that_was_not_an_rc_is_the_first(self):
        for was in ("-beta", "", None):
            self.assertEqual(r.rc_after(was), "-rc")
    def test_a_bad_label_is_refused(self):
        with self.assertRaises(SystemExit): r.relabel(self.v(""), "gold")

class Notes(unittest.TestCase):
    def test_parse(self):
        n = r.parse_unreleased(LOG)
        self.assertEqual(n["summary"], "Faster boot, and a fix for a hang.")
        # The fixture uses the names from before v2.01-beta, read as aliases.
        self.assertEqual(n["new"], ["A thing that was added and wrapped onto a second line."])
        self.assertEqual(n["fixed"], ["A hang", "Another"])
        self.assertEqual(n["issues"], ["Something open"])
        self.assertEqual(n["changed"], [])
        self.assertEqual(r.check_notes(n), [])
    def test_an_empty_template_is_not_ready(self):
        empty = LOG.replace("Faster boot, and a fix for a hang.", "").replace("- A thing", "").replace("- A hang", "").replace("- Another", "")
        self.assertTrue(r.check_notes(r.parse_unreleased(empty)))
    def test_a_summary_with_nothing_listed_is_not_ready(self):
        p = r.check_notes({"summary": "Something happened.", "new": [], "improved": [], "fixed": [], "security": [], "changed": [], "issues": ["open"]})
        self.assertTrue(any("nothing" in x for x in p))
    def test_a_placeholder_summary_is_not_ready(self):
        self.assertTrue(r.check_notes({"summary": "[what changed]", "new": ["x"], "improved": [], "fixed": [], "security": [], "changed": [], "issues": []}))
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

class Categories(unittest.TestCase):
    STD = LOG.replace("### Added", "### New").replace("### Known limitations", "### Known issues")

    def test_every_category_parses(self):
        text = "## Unreleased\n\nSummary: Things.\n\n" + "".join(
            "### %s\n\n- one %s\n\n" % (label, key) for key, label in r.SECTIONS)
        n = r.parse_unreleased(text)
        for key, _ in r.SECTIONS:
            self.assertEqual(n[key], ["one " + key])
        self.assertEqual(r.check_notes(n), [])

    def test_a_heading_that_is_not_a_category_stops_the_release(self):
        text = self.STD.replace("### Fixed", "### Fixed (unreleased)")
        n = r.parse_unreleased(text)
        self.assertTrue(any("not categories" in p for p in r.check_notes(n)))

    def test_known_issues_alone_is_not_a_release(self):
        n = r.parse_unreleased("## Unreleased\n\nSummary: Hm.\n\n### Known issues\n\n- open\n")
        self.assertTrue(r.check_notes(n))

    def test_the_fresh_section_has_every_category_in_order(self):
        out = r.roll_changelog(self.STD, "v2.01-beta (build 600) - 2026-11-20")
        fresh = out[out.index("## Unreleased"):out.index("## v2.01-beta")]
        heads = [l[4:] for l in fresh.splitlines() if l.startswith("### ")]
        self.assertEqual(heads, [label for _, label in r.SECTIONS])

    def test_notes_render_in_category_order(self):
        text = "## Unreleased\n\nSummary: S.\n\n### Fixed\n\n- f\n\n### Security\n\n- s\n\n### New\n\n- n\n"
        n = r.parse_unreleased(text)
        body = r.render_notes({"summary": "S.", "notes": n, "assets": []}, {"boards": []})
        self.assertLess(body.index("### New"), body.index("### Fixed"))
        self.assertLess(body.index("### Fixed"), body.index("### Security"))


def uf2(payload):
    """Wrap bytes in UF2 blocks the way the SDK's tools do: 256 bytes per 512-byte block."""
    out = b""
    chunks = [payload[i:i + 256] for i in range(0, len(payload), 256)]
    for n, c in enumerate(chunks):
        hdr = (b"UF2\n" + (0x9E5D5157).to_bytes(4, "little") + (0).to_bytes(4, "little")
               + (0x10000000 + 256 * n).to_bytes(4, "little") + len(c).to_bytes(4, "little")
               + n.to_bytes(4, "little") + len(chunks).to_bytes(4, "little") + (0).to_bytes(4, "little"))
        out += hdr + c.ljust(476, b"\0") + (0x0AB16F30).to_bytes(4, "little")
    return out


class OnlyTheStandardImage(unittest.TestCase):
    """A custom build (fewer features, the development packages, the emulator) is refused
    whatever its file is called: the variant is read out of the image."""
    def image(self, d, name, body):
        p = os.path.join(d, name)
        with open(p, "wb") as fh:
            fh.write(body)
        return p

    def test_the_standard_image_passes(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.image(d, "a.bin", b"\0" * 300 + b"rpcimage:standard\0" + b"\xff" * 300)
            self.assertIsNone(r.not_standard("pico2_w", p))

    def test_a_custom_image_is_refused_and_says_what_it_is(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.image(d, "rpcortex-v2-pico2_w.bin", b"\0" * 300 + b"rpcimage:nobt\0")
            why = r.not_standard("pico2_w", p)
        self.assertIn("custom image (nobt)", why)
        self.assertIn("./build.sh pico2_w", why)

    def test_an_image_with_no_marker_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.image(d, "a.bin", b"\0" * 600)
            self.assertIn("no feature marker", r.not_standard("pico", p))

    def test_the_uf2_is_read_through_its_blocks(self):
        # The marker straddles the boundary between the first two blocks, so only an
        # unwrapped read can find it.
        body = b"\0" * 250 + b"rpcimage:emu\0" + b"\xff" * 300
        with tempfile.TemporaryDirectory() as d:
            p = self.image(d, "a.uf2", uf2(body))
            self.assertIn("custom image (emu)", r.not_standard("pico", p))
            p = self.image(d, "b.uf2", uf2(body.replace(b"rpcimage:emu", b"rpcimage:standard")))
            self.assertIsNone(r.not_standard("pico", p))


if __name__ == "__main__":
    unittest.main()
