#!/usr/bin/env python3
"""Checks for tools/features.py: what a board's standard image has, what --with and
--without turn into, what a custom image is called, and the marker a release reads."""
import json, os, shlex, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import features as F

HERE = os.path.dirname(os.path.abspath(__file__))


def run(*args):
    p = subprocess.run([sys.executable, os.path.join(HERE, "features.py")] + list(args),
                       capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


class Standard(unittest.TestCase):
    def test_every_board_gets_what_it_can_have(self):
        self.assertEqual(F.standard("pico2_w"), ["wifi", "bluetooth", "ssh", "sd", "peripherals"])
        self.assertEqual(F.standard("pico_w"), ["wifi", "bluetooth", "peripherals"])
        self.assertEqual(F.standard("pico2"), ["sd", "peripherals"])
        self.assertEqual(F.standard("pico"), ["peripherals"])

    def test_the_standard_image_has_no_name_of_its_own(self):
        for b in F.BOARDS:
            self.assertEqual(F.variant(b, F.standard(b)), "")

    def test_development_packages_are_never_standard(self):
        for b in F.BOARDS:
            self.assertNotIn("devpkgs", F.standard(b))

    def test_plain_boards_cannot_have_a_radio_or_ssh(self):
        for b in ("pico", "pico2"):
            for f in ("wifi", "bluetooth", "ssh"):
                with self.assertRaises(F.FeatureError):
                    F.resolve(b, with_=f)

    def test_an_unknown_board_or_feature_is_refused(self):
        with self.assertRaises(F.FeatureError):
            F.standard("esp32s3")
        with self.assertRaises(F.FeatureError):
            F.resolve("pico2_w", without="lidar")


class Resolve(unittest.TestCase):
    def test_leaving_out_bluetooth(self):
        chosen, notes = F.resolve("pico2_w", without="bluetooth")
        self.assertEqual(chosen, ["wifi", "ssh", "sd", "peripherals"])
        self.assertEqual(F.variant("pico2_w", chosen), "nobt")
        self.assertEqual(notes, [])

    def test_leaving_out_wifi_takes_bluetooth_and_ssh_with_it(self):
        chosen, _ = F.resolve("pico2_w", without="wifi")
        self.assertEqual(chosen, ["sd", "peripherals"])
        # -nowifi already says there is no Bluetooth or SSH; they are not named twice.
        self.assertEqual(F.variant("pico2_w", chosen), "nowifi")

    def test_bluetooth_without_wifi_is_refused(self):
        with self.assertRaises(F.FeatureError):
            F.resolve("pico_w", with_="bluetooth", without="wifi")

    def test_both_radios_named(self):
        chosen, _ = F.resolve("pico_w", without="wifi,bluetooth")
        self.assertEqual(chosen, ["peripherals"])
        self.assertEqual(F.variant("pico_w", chosen), "nowifi")

    def test_names_are_forgiving(self):
        a, _ = F.resolve("pico2_w", without=["BT", "Wi-Fi"])
        b, _ = F.resolve("pico2_w", without="bluetooth,wifi")
        c, _ = F.resolve("pico2_w", without=" wifi , bluetooth ")
        self.assertEqual(a, b)
        self.assertEqual(b, c)

    def test_ssh_without_wifi_is_refused_when_asked_for(self):
        with self.assertRaises(F.FeatureError):
            F.resolve("pico2_w", with_="ssh", without="wifi")

    def test_ssh_without_wifi_is_switched_off_for_cmake(self):
        chosen, notes = F.resolve("pico2_w", with_="ssh", without="wifi", strict=False)
        self.assertNotIn("ssh", chosen)
        self.assertTrue(any("ssh" in n for n in notes))

    def test_ssh_can_be_added_on_a_pico_w(self):
        chosen, _ = F.resolve("pico_w", with_="ssh")
        self.assertIn("ssh", chosen)
        self.assertEqual(F.variant("pico_w", chosen), "ssh")

    def test_asking_both_ways_is_refused(self):
        with self.assertRaises(F.FeatureError):
            F.resolve("pico2_w", with_="sd", without="sd")

    def test_leaving_out_what_a_board_does_not_have_is_a_note_not_an_error(self):
        chosen, notes = F.resolve("pico", without="wifi")
        self.assertEqual(chosen, F.standard("pico"))
        self.assertEqual(F.variant("pico", chosen), "")
        self.assertEqual(len(notes), 1)

    def test_adding_the_development_packages(self):
        chosen, _ = F.resolve("pico", with_="dev")
        self.assertEqual(F.variant("pico", chosen), "dev")
        chosen, _ = F.resolve("pico2_w", with_="devpkgs", without="peripherals")
        self.assertEqual(F.variant("pico2_w", chosen), "noperiph-dev")

    def test_every_option_reaches_cmake(self):
        args = F.cmake_args(F.resolve("pico2_w", without="bluetooth")[0])
        self.assertEqual(len(args), len(F.FEATURES))
        self.assertIn("-DRPC_FEATURE_BLUETOOTH=OFF", args)
        self.assertIn("-DRPC_FEATURE_WIFI=ON", args)
        self.assertIn("-DRPC_DEV_PACKAGES=OFF", args)


class ForBuildSh(unittest.TestCase):
    def test_resolve_prints_what_build_sh_evaluates(self):
        rc, out, err = run("resolve", "pico2_w", "--with", "", "--without", ",bluetooth")
        self.assertEqual(rc, 0, err)
        env = dict(line.split("=", 1) for line in out.strip().splitlines())
        self.assertEqual(shlex.split(env["FEAT_TAG"]), ["nobt"])
        self.assertIn("-DRPC_FEATURE_BLUETOOTH=OFF", shlex.split(env["FEAT_CMAKE"])[0].split())

    def test_resolve_fails_with_a_reason(self):
        rc, out, err = run("resolve", "pico", "--with", "wifi")
        self.assertNotEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertIn("wifi cannot be built for pico", err)

    def test_cmake_writes_settings_and_defaults_what_it_was_not_given(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "features.cmake")
            rc, _, err = run("cmake", "pico_w", out, "RPC_FEATURE_BLUETOOTH=OFF")
            self.assertEqual(rc, 0, err)
            with open(out) as fh:
                text = fh.read()
        self.assertIn("set(RPC_FEATURE_WIFI ON)", text)
        self.assertIn("set(RPC_FEATURE_BLUETOOTH OFF)", text)
        self.assertIn("set(RPC_FEATURE_SD OFF)", text)
        self.assertIn("set(RPC_CAN_SD 0)", text)
        self.assertIn("set(RPC_CAN_WIFI 1)", text)
        self.assertIn("set(RPC_DEV_PACKAGES OFF)", text)
        self.assertIn('set(RPC_FEATURE_VARIANT "nobt")', text)
        # SSH is the option the SSH work added, RPC_SSH; a Pico W can have it but its
        # standard image does not, which is what decides whether `ssh` explains itself.
        self.assertIn("set(RPC_SSH OFF)", text)
        self.assertIn("set(RPC_CAN_SSH 1)", text)
        self.assertIn("set(RPC_STD_SSH 0)", text)

    def test_without_wifi_cmake_gets_ssh_off(self):
        args = F.cmake_args(F.resolve("pico2_w", without="wifi")[0])
        self.assertIn("-DRPC_SSH=OFF", args)
        self.assertIn("-DRPC_SSH=ON", F.cmake_args(F.standard("pico2_w")))

    def test_cmake_refuses_a_feature_the_board_cannot_have(self):
        with tempfile.TemporaryDirectory() as d:
            rc, _, err = run("cmake", "pico2", os.path.join(d, "f.cmake"), "RPC_FEATURE_WIFI=ON")
        self.assertNotEqual(rc, 0)
        self.assertIn("wifi", err)


class Marker(unittest.TestCase):
    def test_reads_the_variant_out_of_an_image(self):
        img = b"\x00" * 100 + b"rpcimage:nowifi-nobt\x00more" + b"\xff" * 10
        self.assertEqual(F.image_variant(img), "nowifi-nobt")
        self.assertEqual(F.image_variant(b"xx rpcimage:standard\x00"), "standard")

    def test_an_image_without_a_marker(self):
        self.assertIsNone(F.image_variant(b"\x00" * 64))


class Sizes(unittest.TestCase):
    def test_saving_is_standard_minus_variant(self):
        s = {"boards": {"pico_w": {"standard": {"flash": 1000, "ram": 500},
                                   "variants": {"nobt": {"flash": 600, "ram": 300}}}}}
        self.assertEqual(F.saving(s, "pico_w", "nobt"), (400, 200))
        self.assertIsNone(F.saving(s, "pico_w", "nowifi"))
        self.assertIsNone(F.saving(None, "pico", "noperiph"))

    def test_headroom_and_an_image_that_does_not_fit(self):
        s = {"boards": {"pico_w": {"standard": {"flash": 820 * 1024, "ram": 0, "slot": 832 * 1024},
                                   "variants": {"nobt": {"flash": 700 * 1024, "ram": 0},
                                                "dev": {"flash": 851 * 1024, "ram": 0, "fits": False}}}}}
        self.assertIn("12 KB -> 132 KB", F.describe_saving(s, "pico_w", F.feature("bluetooth")))
        dev = F.describe_saving(s, "pico_w", F.feature("devpkgs"))
        self.assertIn("adds 31 KB", dev)
        self.assertIn("does NOT fit", dev)

    def test_the_list_prints_without_sizes(self):
        rc, out, err = run("list")
        self.assertEqual(rc, 0, err)
        for f in F.FEATURES:
            self.assertIn(f.name, out)


class BuildConf(unittest.TestCase):
    """tools/buildconf.py, answered through a pipe, never told to build."""
    def answers(self, text, sizes=None, rc=0):
        # No measured sizes unless a test gives some, so the answers do not depend on
        # the last `tools/features.py measure`.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "sizes.json")
            if sizes is not None:
                with open(path, "w") as fh:
                    json.dump(sizes, fh)
            p = subprocess.run([sys.executable, os.path.join(HERE, "buildconf.py")], input=text,
                               capture_output=True, text=True, timeout=30,
                               env=dict(os.environ, RPC_FEATURE_SIZES=path))
        self.assertEqual(p.returncode, rc, p.stdout + p.stderr)
        lines = p.stdout.splitlines()
        return lines[lines.index("The command:") + 1].strip(), p.stdout

    def test_enter_everywhere_is_the_standard_image(self):
        cmd, out = self.answers("\n" * 12)
        self.assertEqual(cmd, "./build.sh pico2_w")
        self.assertIn("the standard pico2_w image", out)
        self.assertIn("Not built", out)

    def test_leaving_out_bluetooth_on_a_pico_w(self):
        # board 2; keep Wi-Fi; no Bluetooth; no SSH (not standard there); keep the
        # drivers; no dev packages; do not build
        cmd, out = self.answers("2\n\nn\n\n\nn\nn\n")
        self.assertEqual(cmd, "./build.sh pico_w --without bluetooth")
        self.assertIn("rpcortex-v2-pico_w-nobt.uf2", out)
        self.assertIn("Add SSH?", out)

    def test_adding_ssh_on_a_pico_w(self):
        cmd, _ = self.answers("pico_w\n\n\ny\n\n\nn\n")
        self.assertEqual(cmd, "./build.sh pico_w --with ssh")

    def test_an_image_that_will_not_fit_is_not_offered(self):
        k = 1024
        sizes = {"measured_at": "a test", "boards": {"pico_w": {
            "standard": {"flash": 820 * k, "ram": 0, "slot": 832 * k},
            "variants": {"ssh": {"flash": 852 * k, "ram": 0, "fits": False}}}}}
        # Pico W with SSH added; nothing else changed. No "Build it now?" answer is
        # given, and none is needed: it is not offered.
        cmd, out = self.answers("pico_w\n\n\ny\n\n\n", sizes=sizes, rc=1)
        self.assertEqual(cmd, "./build.sh pico_w --with ssh")
        self.assertIn("will refuse it", out)
        self.assertNotIn("Build it now?", out)

    def test_no_wifi_means_no_ssh_question(self):
        cmd, out = self.answers("pico2_w\nn\n\n\n\n\nn\n")
        self.assertEqual(cmd, "./build.sh pico2_w --without wifi")
        self.assertNotIn("Keep SSH?", out)

    def test_adding_and_removing_on_a_pico(self):
        cmd, _ = self.answers("4\nn\ny\nn\n")
        self.assertEqual(cmd, "./build.sh pico --with devpkgs --without peripherals")


if __name__ == "__main__":
    unittest.main()
