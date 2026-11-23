"""`rpc backup --with-system` and the restore that puts the system files back:
the settings, the accounts, the device key and the SSH host key, read and
written with sudo, confirmed first, and followed by a restart that saves
nothing (a plain `reboot` would write the in-memory settings over them)."""
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402,F401
from support import run_cli  # noqa: E402
from tui_mock import TuiMock, wait_for  # noqa: E402

PASSWORD = "rootpw"


class TestCliSystemBackup(unittest.TestCase):
    env = {"RPC_PASSWORD": PASSWORD}

    def setUp(self):
        self.dev = TuiMock().start()
        self.addCleanup(self.dev.shutdown)
        self.tmp = tempfile.mkdtemp(prefix="rpcbk-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.dest = os.path.join(self.tmp, "bk.zip")

    def test_default_lists_what_was_left_out(self):
        rc, out, err = run_cli(self.dev, ["backup", self.dest], env=self.env)
        self.assertEqual(rc, 0, err)
        text = out.decode()
        self.assertIn("Not included:", text)
        self.assertIn("(add with --with-system)", text)
        self.assertIn("(add with --with-packages)", text)
        self.assertFalse(any(n.startswith("system/") for n in zipfile.ZipFile(self.dest).namelist()))

    def test_with_system_and_restore(self):
        rc, out, err = run_cli(self.dev, ["backup", "--with-system", self.dest], env=self.env)
        self.assertEqual(rc, 0, err)
        names = zipfile.ZipFile(self.dest).namelist()
        self.assertIn("system/os/device.key", names)
        self.assertFalse(any(n.startswith("files/os/") for n in names),
                         "system files sit apart, where an older restore does not look")
        man = json.loads(zipfile.ZipFile(self.dest).read("rpc-backup.json"))
        self.assertEqual(sorted(e["path"] for e in man["system"]),
                         ["/os/device.key", "/os/registry.cfg", "/os/ssh/host_ed25519",
                          "/os/users.cfg"])
        self.assertEqual(self.dev.auths, [PASSWORD], "the keys are read with sudo, asked once")
        self.assertNotIn("--with-system)", out.decode())
        self.assertIn("Keep it as safe", err)

        fresh = TuiMock().start()
        self.addCleanup(fresh.shutdown)
        fresh.fs.files["/os/device.key"] = b"\x09" * 32
        fresh.fs.files["/os/users.cfg"] = fresh.fs.files["/os/users.bak"] = b"other:hash\n"
        rc, out, err = run_cli(fresh, ["restore", self.dest], env=self.env)
        self.assertEqual(rc, 0, err)
        self.assertIn("left as they are", err, "not asked without a terminal or --yes")
        self.assertEqual(fresh.fs.files["/os/device.key"], b"\x09" * 32)
        self.assertEqual(fresh.reboots, [])

        rc, out, err = run_cli(fresh, ["restore", "--yes", self.dest], env=self.env)
        self.assertEqual(rc, 0, err)
        for want in ("/os/device.key", "/os/users.cfg", "different device"):
            self.assertIn(want, err, "the confirmation names each file and the key's catch")
        self.assertEqual(fresh.fs.files["/os/device.key"], b"\x01" * 32)
        self.assertEqual(fresh.fs.files["/os/users.cfg"], self.dev.fs.files["/os/users.cfg"])
        self.assertEqual(fresh.fs.files["/os/users.bak"], self.dev.fs.files["/os/users.cfg"],
                         "the shadow copy agrees with the restored file")
        self.assertTrue(wait_for(lambda: fresh.reboots == ["sreboot"]),
                        "restarted without saving, not with reboot")
        self.assertIn("restarting", out.decode())

    def test_skip_system_and_dry_run(self):
        run_cli(self.dev, ["backup", "--with-system", self.dest], env=self.env)
        rc, out, err = run_cli(self.dev, ["restore", "--dry-run", self.dest], env=self.env)
        self.assertIn("would replace /os/users.cfg", err)
        self.assertEqual(self.dev.reboots, [])
        rc, out, err = run_cli(self.dev, ["restore", "--skip-system", "--yes", self.dest],
                               env=self.env)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.reboots, [])

    def test_missing_ssh_is_not_a_failure(self):
        del self.dev.fs.files["/os/ssh/host_ed25519"]
        self.dev.fs.dirs.discard("/os/ssh")
        rc, out, err = run_cli(self.dev, ["backup", "--with-system", self.dest], env=self.env)
        self.assertEqual(rc, 0, err)
        self.assertNotIn("skipped", out.decode())

    def test_guest_cannot(self):
        self.dev.user = "guest"
        rc, out, err = run_cli(self.dev, ["backup", "--with-system", self.dest], env=self.env)
        self.assertEqual(rc, 1)
        self.assertIn("skipped /os/users.cfg", err)


if __name__ == "__main__":
    unittest.main()
